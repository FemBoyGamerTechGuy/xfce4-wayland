/* test_seat.c — seat/session provider coverage (Phase 4).
 *
 * The built-in seatd wire-protocol client is tested against a REAL
 * mock seatd server (a forked child speaking the documented seatd 0.9
 * protocol over a unix socket, passing device fds via SCM_RIGHTS) —
 * handshake, device open/close, session switch, the DISABLE -> ack ->
 * ENABLE lifecycle, server-side errors and out-of-sync connections.
 *
 * When this build has libseat, the same mock server is also used to
 * open a seat through libseat itself (LIBSEAT_BACKEND=seatd): the real
 * upstream client and our built-in client must interoperate with the
 * same server implementation, which cross-validates the mock against
 * the protocol libseat expects (and our libseat wrapper against the
 * real library).
 *
 * The direct-VT provider is only probed for its failure diagnostic when
 * the test environment has no virtual terminal (containers); on a real
 * TTY the test would otherwise take the terminal over.
 */
#include "xwtest.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/vt.h>
#include <sys/wait.h>
#include <unistd.h>

/* seatd wire protocol (upstream include/protocol.h values) */
enum {
    CL_OPEN_SEAT = 1,
    CL_CLOSE_SEAT = 2,
    CL_OPEN_DEVICE = 3,
    CL_CLOSE_DEVICE = 4,
    CL_DISABLE_SEAT = 5,
    CL_SWITCH_SESSION = 6,
};
enum {
    SV_SEAT_OPENED = 0x8001,
    SV_SEAT_CLOSED = 0x8002,
    SV_DEVICE_OPENED = 0x8003,
    SV_DEVICE_CLOSED = 0x8004,
    SV_DISABLE_SEAT = 0x8005,
    SV_ENABLE_SEAT = 0x8006,
    SV_SESSION_SWITCHED = 0x8008,
    SV_SEAT_DISABLED = 0x8009,
    SV_ERROR = 0x7FFF,
};

struct msg {
    uint16_t opcode;
    uint16_t size;
};

static void send_msg(int fd, uint16_t opcode, const void *payload,
                     uint16_t size) {
    struct msg m = {opcode, size};
    (void)!write(fd, &m, sizeof(m));
    if (size)
        (void)!write(fd, payload, size);
}

/* send DEVICE_OPENED with an fd attached via SCM_RIGHTS */
static void send_fd_msg(int fd, int dev_fd) {
    struct msg m = {SV_DEVICE_OPENED, 4};
    int32_t dev_id = 7;
    struct iovec iov[2] = {
        {.iov_base = &m, .iov_len = sizeof(m)},
        {.iov_base = &dev_id, .iov_len = sizeof(dev_id)},
    };
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr mh = {0};
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &dev_fd, sizeof(int));
    (void)!sendmsg(fd, &mh, 0);
}

/* read one full message; returns opcode, payload copied out (small) */
static int read_msg(int fd, void *payload, size_t cap) {
    struct msg m;
    size_t got = 0;
    while (got < sizeof(m)) {
        ssize_t n = read(fd, (char *)&m + got, sizeof(m) - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    got = 0;
    while (got < m.size && got < cap) {
        ssize_t n = read(fd, (char *)payload + got, m.size - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    /* drain any payload beyond the capture buffer */
    while (got < m.size) {
        char scratch[256];
        size_t want = m.size - got;
        if (want > sizeof(scratch))
            want = sizeof(scratch);
        ssize_t n = read(fd, scratch, want);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return m.opcode;
}

/* ------------------------------------------------------------ mock server */

/* modes:
 *   ok            full well-behaved server
 *   disable       handshake, then: on the first client request (an
 *                 OPEN_DEVICE) send DISABLE as background traffic,
 *                 answer the request, wait for the client's ack,
 *                 SEAT_DISABLED + ENABLE, then keep serving
 *   error         OPEN_SEAT -> SERVER_ERROR(EPERM)
 *   garbage       OPEN_SEAT -> out-of-sync junk
 *   close-early   accept + close immediately
 */
static int mock_seatd_run(const char *sock, const char *mode, int ready_fd) {
    unlink(sock);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
        _exit(101);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        _exit(102);
    if (listen(lfd, 4) < 0)
        _exit(103);
    /* tell the parent we are listening */
    char ok = 'y';
    (void)!write(ready_fd, &ok, 1);
    close(ready_fd);

    int fd = accept(lfd, NULL, NULL);
    close(lfd);
    if (fd < 0)
        _exit(104);

    char payload[256];

    if (strcmp(mode, "close-early") == 0) {
        close(fd);
        _exit(0);
    }

    /* handshake */
    if (read_msg(fd, payload, sizeof(payload)) != CL_OPEN_SEAT) {
        close(fd);
        _exit(110);
    }
    if (strcmp(mode, "error") == 0) {
        int32_t e = EPERM;
        send_msg(fd, SV_ERROR, &e, 4);
        close(fd);
        _exit(0);
    }
    if (strcmp(mode, "garbage") == 0) {
        const char junk[] = "this is not a seatd message at all";
        (void)!write(fd, junk, sizeof(junk));
        usleep(100000); /* let the client read it */
        close(fd);
        _exit(0);
    }

    const char name[] = "seat-mock";
    uint16_t name_len = (uint16_t)(sizeof(name)); /* includes the NUL */
    char open_payload[64];
    memcpy(open_payload, &name_len, 2);
    memcpy(open_payload + 2, name, sizeof(name));
    send_msg(fd, SV_SEAT_OPENED, open_payload, (uint16_t)(2 + sizeof(name)));

    if (strcmp(mode, "disable") == 0) {
        /* wait for the first request (OPEN_DEVICE from the test), send
         * DISABLE as background traffic BEHIND that request's response,
         * then complete the ack dance */
        int op = read_msg(fd, payload, sizeof(payload));
        if (op != CL_OPEN_DEVICE)
            _exit(112);
        send_msg(fd, SV_DISABLE_SEAT, NULL, 0);
        int dfd = open("/dev/null", O_RDWR | O_CLOEXEC);
        send_fd_msg(fd, dfd);
        close(dfd);
        for (;;) {
            op = read_msg(fd, payload, sizeof(payload));
            if (op == CL_DISABLE_SEAT)
                break;
            if (op < 0)
                _exit(111);
        }
        send_msg(fd, SV_SEAT_DISABLED, NULL, 0);
        send_msg(fd, SV_ENABLE_SEAT, NULL, 0);
    }

    /* serve until CLOSE_SEAT (or EOF) */
    int exit_code = 0;
    for (;;) {
        int op = read_msg(fd, payload, sizeof(payload));
        if (op < 0)
            break;
        switch (op) {
        case CL_OPEN_DEVICE: {
            /* payload: u16 path_len + path (NUL included) */
            uint16_t plen;
            memcpy(&plen, payload, 2);
            char path[256] = {0};
            if (plen > 0 && plen <= sizeof(path) - 1)
                memcpy(path, payload + 2, plen - 1);
            int dfd = open(path[0] ? path : "/dev/null", O_RDWR | O_CLOEXEC);
            if (dfd < 0)
                dfd = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (dfd < 0) {
                int32_t e = errno ? errno : EIO;
                send_msg(fd, SV_ERROR, &e, 4);
                break;
            }
            send_fd_msg(fd, dfd);
            close(dfd); /* the passed copy stays open in the client */
            break;
        }
        case CL_CLOSE_DEVICE:
            send_msg(fd, SV_DEVICE_CLOSED, NULL, 0);
            break;
        case CL_SWITCH_SESSION:
            send_msg(fd, SV_SESSION_SWITCHED, NULL, 0);
            break;
        case CL_DISABLE_SEAT:
            send_msg(fd, SV_SEAT_DISABLED, NULL, 0);
            break;
        case CL_CLOSE_SEAT:
            send_msg(fd, SV_SEAT_CLOSED, NULL, 0);
            exit_code = 0;
            goto done;
        default:
            /* unknown client opcode: real seatd errors out; so do we */
            send_msg(fd, SV_ERROR, &(int32_t){EBADMSG}, 4);
            exit_code = 120;
            goto done;
        }
    }
done:
    close(fd);
    _exit(exit_code);
}

/* start the mock; returns pid. Socket path written to *sock_out. */
static pid_t mock_start(char *mode, char *sock_out, size_t sock_cap) {
    snprintf(sock_out, sock_cap, "%s/seatd-mock-%d.sock", g_runtimedir(),
             (int)getpid());
    int ready[2];
    if (pipe(ready) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    if (pid == 0) {
        close(ready[0]);
        mock_seatd_run(sock_out, mode, ready[1]);
        _exit(125); /* not reached */
    }
    close(ready[1]);
    /* wait for the listening signal (with timeout) */
    struct pollfd pfd = {.fd = ready[0], .events = POLLIN};
    if (poll(&pfd, 1, 5000) != 1) {
        close(ready[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    char c;
    (void)!read(ready[0], &c, 1);
    close(ready[0]);
    return pid;
}

static int mock_wait(pid_t pid) {
    int status = 0;
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return status;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return status;
}

/* --------------------------------------------------------------- helpers */

static struct xw_seat_session *open_mock_seat(struct xwt_ctx *t,
                                              const char *mode, pid_t *pid_out,
                                              char *sock_out) {
    pid_t pid = mock_start((char *)mode, sock_out, 108);
    if (pid <= 0) {
        XWT_CHECK(false, "the mock seatd server did not start");
        *pid_out = -1;
        return NULL;
    }
    setenv("SEATD_SOCK", sock_out, 1);
    unsetenv("LIBSEAT_BACKEND"); /* the built-in client, not libseat */
    struct xw_seat_session *s =
        xw_seat_session_open(t->comp, XW_SEAT_PROVIDER_SEATD, "seat-mock");
    *pid_out = pid;
    return s;
}

static void close_mock_env(const char *sock) {
    unsetenv("SEATD_SOCK");
    unlink(sock);
}

/* ------------------------------------------------------------------ tests */

static void test_seatd_handshake(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "ok", &pid, sock);
    XWT_ASSERT(s != NULL);
    XWT_CHECK(strcmp(xw_seat_session_name(s), "seat-mock") == 0,
              "seat name round-trips through the wire protocol");
    XWT_CHECK(strstr(xw_seat_session_desc(s), "seatd") != NULL,
              "provider description names seatd");
    XWT_CHECK(xw_seat_session_active(s), "seat starts active");
    xw_seat_session_destroy(s);

    int st = mock_wait(pid);
    XWT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "mock server saw CLOSE_SEAT and exited cleanly");
    close_mock_env(sock);
}

static void test_seatd_open_device(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "ok", &pid, sock);
    XWT_ASSERT(s != NULL);

    int fd = -1;
    int dev_id = xw_seat_session_open_device(s, "/dev/null", &fd);
    XWT_CHECK(dev_id >= 0, "open_device returns a device id (got %d)", dev_id);
    XWT_CHECK(fd >= 0 && fcntl(fd, F_GETFD) >= 0,
              "the device fd arrived through SCM_RIGHTS and is valid");
    if (fd >= 0) {
        char zero = 0;
        XWT_CHECK(write(fd, &zero, 0) == 0, "the fd is a real writable fd");
    }
    XWT_CHECK(xw_seat_session_close_device(s, dev_id) == 0,
              "close_device round-trips");

    /* a second open/close cycle (the mock hands out fresh fds) */
    int fd2 = -1;
    int dev2 = xw_seat_session_open_device(s, "/dev/zero", &fd2);
    XWT_CHECK(dev2 >= 0 && fd2 >= 0, "second open works");
    if (dev2 >= 0)
        xw_seat_session_close_device(s, dev2);
    if (fd2 >= 0)
        close(fd2);
    if (fd >= 0)
        close(fd);

    xw_seat_session_destroy(s);
    mock_wait(pid);
    close_mock_env(sock);
}

static int g_disable_events, g_enable_events;

static void on_disable(void *ud) {
    (void)ud;
    g_disable_events++;
}

static void on_enable(void *ud) {
    (void)ud;
    g_enable_events++;
}

static void test_seatd_disable_lifecycle(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "disable", &pid, sock);
    XWT_ASSERT(s != NULL);

    g_disable_events = 0;
    g_enable_events = 0;
    struct xw_seat_events ev = {
        .disable = on_disable,
        .enable = on_enable,
    };
    xw_seat_session_set_events(s, &ev, NULL);

    /* the first device open carries the DISABLE event as background
     * traffic: it must fire synchronously, before the request returns
     * (the compositor releases scanout resources while the request is
     * still blocking) */
    int fd = -1;
    int dev_id = xw_seat_session_open_device(s, "/dev/null", &fd);
    XWT_CHECK(dev_id >= 0 && fd >= 0, "the request itself still succeeds");
    XWT_CHECK(g_disable_events == 1,
              "disable event delivered behind the request (%d)",
              g_disable_events);
    XWT_CHECK(!xw_seat_session_active(s), "seat is inactive after disable");

    /* acknowledging lets the mock finish the cycle and send ENABLE */
    XWT_CHECK(xw_seat_session_ack_disable(s) == 0, "ack round-trips");
    XWT_CHECK(g_enable_events == 1,
              "enable event delivered after the ack (%d)", g_enable_events);
    XWT_CHECK(xw_seat_session_active(s), "seat is active again after enable");

    if (dev_id >= 0)
        xw_seat_session_close_device(s, dev_id);
    if (fd >= 0)
        close(fd);
    xw_seat_session_destroy(s);
    int st = mock_wait(pid);
    XWT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "mock server completed the disable/ack/enable dance");
    close_mock_env(sock);
}

/* The consumer-less disable MUST auto-ack: the seatd daemon holds its
 * VT handoff until the DISABLE_SEAT ack arrives, and a seat opened
 * without the DRM backend registering hooks would otherwise park the
 * console mid-switch forever (the VT-trap failure mode). The mock's
 * disable dance completes (ack received -> SEAT_DISABLED -> ENABLE)
 * without the test acknowledging anything. */
static void test_seatd_disable_autoack(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "disable", &pid, sock);
    XWT_ASSERT(s != NULL);

    g_disable_events = 0;
    g_enable_events = 0;
    /* events registered WITHOUT a disable consumer: seat_call_disable
     * must ack on its own */
    struct xw_seat_events ev = {
        .disable = NULL,
        .enable = on_enable,
    };
    xw_seat_session_set_events(s, &ev, NULL);

    /* the first device open carries the DISABLE event; the auto-ack
     * lets the mock complete the cycle and send ENABLE */
    int fd = -1;
    int dev_id = xw_seat_session_open_device(s, "/dev/null", &fd);
    XWT_CHECK(dev_id >= 0 && fd >= 0, "the request itself still succeeds");
    XWT_CHECK(g_enable_events == 1,
              "enable delivered after the automatic ack (%d)",
              g_enable_events);
    XWT_CHECK(xw_seat_session_active(s),
              "the whole disable/ack/enable cycle completed on its own");

    if (dev_id >= 0)
        xw_seat_session_close_device(s, dev_id);
    if (fd >= 0)
        close(fd);
    xw_seat_session_destroy(s);
    int st = mock_wait(pid);
    XWT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "mock saw the ack without any test-side ack call");
    close_mock_env(sock);
}

/* Ctrl+Alt+F1..F12 (raw linux keycodes 59..68, 87, 88) requests a VT
 * switch through the seat session and is consumed like a shortcut —
 * the focused client never sees the key. F-keys without Ctrl+Alt pass
 * through to clients. The headless test compositor gets a mock seat
 * session so the DRM-only path is exercisable without hardware. */
static int g_vt_key_count;
static struct xwc_win *g_vt_win;
static void vt_key_cb(struct xwc_win *w, uint32_t keycode, bool down,
                      xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)sym;
    (void)mods;
    (void)ud;
    /* count F2 presses only: the Ctrl/Alt modifier keys themselves are
     * legitimately delivered to the client while held */
    if (w == g_vt_win && down && keycode == 60)
        g_vt_key_count++;
}

/* solid-fill configure so the window maps and takes focus */
static void vt_win_configure(struct xwc_win *w, int width, int height,
                             void *ud) {
    (void)width;
    (void)height;
    uint32_t color = *(uint32_t *)ud;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, color);
    xwc_win_commit(w);
}

static void test_seat_vt_switch_keys(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "ok", &pid, sock);
    XWT_ASSERT(s != NULL);
    /* the DRM backend is the only seat-session consumer; give the
     * headless test compositor one so the key path is armed */
    XWT_ASSERT(t->comp->seat == NULL);
    t->comp->seat = s;

    /* a focused window counts keys it receives */
    static uint32_t color = 0xff445566;
    g_vt_key_count = 0;
    struct xwc_callbacks cb = {
        .key = vt_key_cb,
        .configure = vt_win_configure,
        .ud = &color,
    };
    g_vt_win = xwc_win_create(&t->client, &cb, "VTKeys", "vtkeys", 300, 200);
    XWT_ASSERT(g_vt_win);
    XWT_WAIT(t, t->comp->wm->focused &&
                    strcmp(t->comp->wm->focused->title, "VTKeys") == 0);

    /* F2 with Ctrl+Alt held: consumed by the VT-switch path (the mock
     * answers the SWITCH_SESSION round trip), never delivered */
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_F2, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_F2, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
    xwt_pump(t);
    XWT_CHECK(g_vt_key_count == 0,
              "Ctrl+Alt+F2 consumed by the VT switch path (%d delivered)",
              g_vt_key_count);

    /* F2 without modifiers: delivered to the client like any key */
    xw_compositor_inject_key(t->comp, K_F2, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_F2, false);
    xwt_pump(t);
    XWT_CHECK(g_vt_key_count == 1,
              "plain F2 reaches the client (%d)", g_vt_key_count);

    xwc_win_destroy(g_vt_win);
    g_vt_win = NULL;
    /* the harness teardown destroys the compositor, which destroys
     * the seat session; the mock exits through its CLOSE_SEAT path */
    t->comp->seat = NULL; /* the test owned it; destroy it directly */
    xw_seat_session_destroy(s);
    mock_wait(pid);
    close_mock_env(sock);
}

/* ------------------------------------------------- keyboard matrix */

/* The physical "Backspace types u" matrix, pinned headlessly: the
 * exact wl_keyboard event stream a raw client must observe for every
 * key combination of the report, plus the keycode space of the
 * delivered keymap itself. The xwc key callback reports the RAW linux
 * keycode (wire - 8) and the keysym IT computed from the delivered
 * keymap + modifiers events — so a double +8, a raw-space keymap, or
 * a corrupted modifier stream all fail here with a diff of exactly
 * which event was wrong. */
#include <xkbcommon/xkbcommon-keysyms.h>

#define K_BKSP 14 /* linux KEY_BACKSPACE */
#define K_U    22 /* linux KEY_U */
#define K_A    30 /* linux KEY_A */

struct krec {
    uint32_t code; /* raw linux keycode as the client received it */
    bool down;
    xkb_keysym_t sym; /* the client's own decode of the wire keycode */
    uint32_t mods;   /* depressed mods at delivery time */
};
#define KREC_MAX 64
static struct krec g_krec[KREC_MAX];
static int g_nkrec;

static void matrix_key_cb(struct xwc_win *w, uint32_t keycode, bool down,
                          xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)w;
    (void)ud;
    if (g_nkrec < KREC_MAX) {
        g_krec[g_nkrec].code = keycode;
        g_krec[g_nkrec].down = down;
        g_krec[g_nkrec].sym = sym;
        g_krec[g_nkrec].mods = mods;
    }
    g_nkrec++;
}

/* solid-fill configure so the window maps and takes focus */
static void matrix_win_configure(struct xwc_win *w, int width, int height,
                                 void *ud) {
    (void)width;
    (void)height;
    uint32_t color = *(uint32_t *)ud;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, color);
    xwc_win_commit(w);
}

/* inject one key press+release pair */
static void tap(struct xwt_ctx *t, uint32_t code) {
    xw_compositor_inject_key(t->comp, code, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, code, false);
    xwt_pump(t);
}

/* press (no release) — for modifier holds */
static void hold(struct xwt_ctx *t, uint32_t code, bool down) {
    xw_compositor_inject_key(t->comp, code, down);
    xwt_pump(t);
}

static void check_krec(struct xwt_ctx *t, int idx, uint32_t code, bool down,
                       xkb_keysym_t sym, const char *what) {
    (void)t; /* context only for the XWT_CHECK bookkeeping */
    XWT_CHECK(idx < g_nkrec, "event %d (%s) never arrived (got %d)", idx,
              what, g_nkrec);
    if (idx >= g_nkrec)
        return;
    XWT_CHECK(g_krec[idx].code == code,
              "event %d (%s): raw keycode %u, expected %u — a wrong "
              "number here means the wire keycode is not evdev+8",
              idx, what, g_krec[idx].code, code);
    XWT_CHECK(g_krec[idx].sym == sym,
              "event %d (%s): client decoded keysym %#x, expected %#x — "
              "the keymap/modifier decode of the delivered events is "
              "wrong (this is the backspace-types-u failure shape)",
              idx, what, g_krec[idx].sym, (unsigned)sym);
    XWT_CHECK(g_krec[idx].down == down, "event %d (%s): state is %s",
              idx, what, g_krec[idx].down ? "press" : "release");
}

static void test_seat_keyboard_matrix(struct xwt_ctx *t) {
    /* 1. the delivered keymap itself: the standard evdev keycode-space
     * spot table. If the keymap were compiled in a raw keycode space
     * (the empty-RMLVO family), wl 22 would decode as 'u' here and
     * every physical Backspace press would type 'u' in every client. */
    XWT_ASSERT(XWT_WAIT(t, t->client.xkb_state != NULL));
    struct {
        uint32_t wire;
        xkb_keysym_t expect;
        const char *what;
    } spot[] = {
        {22, XKB_KEY_BackSpace, "wire 22 = BackSpace (raw 14)"},
        {23, XKB_KEY_Tab, "wire 23 = Tab (raw 15)"},
        {30, XKB_KEY_u, "wire 30 = u (raw 22)"},
        {36, XKB_KEY_Return, "wire 36 = Return (raw 28)"},
        {38, XKB_KEY_a, "wire 38 = a (raw 30)"},
        {50, XKB_KEY_Shift_L, "wire 50 = Shift_L (raw 42)"},
        {37, XKB_KEY_Control_L, "wire 37 = Control_L (raw 29)"},
        {67, XKB_KEY_F1, "wire 67 = F1 (raw 59)"},
    };
    for (size_t i = 0; i < sizeof(spot) / sizeof(spot[0]); i++) {
        xkb_keysym_t got =
            xkb_state_key_get_one_sym(t->client.xkb_state, spot[i].wire);
        XWT_CHECK(got == spot[i].expect,
                  "keymap spot-check %s: got %#x, expected %#x",
                  spot[i].what, (unsigned)got, (unsigned)spot[i].expect);
    }

    /* 2. a focused window records every key event */
    static uint32_t color = 0xff335577;
    g_nkrec = 0;
    struct xwc_callbacks cb = {
        .key = matrix_key_cb,
        .configure = matrix_win_configure,
        .ud = &color,
    };
    struct xwc_win *win =
        xwc_win_create(&t->client, &cb, "KeyMatrix", "keymatrix", 300, 200);
    XWT_ASSERT(win);
    XWT_WAIT(t, t->comp->wm->focused &&
                    strcmp(t->comp->wm->focused->title, "KeyMatrix") == 0);
    XWT_WAIT(t, t->client.has_focus);
    g_nkrec = 0; /* drop focus-time events; the matrix starts clean */

    /* 3. the report matrix, in the physical-test order */
    tap(t, K_BKSP);                                /* plain Backspace */
    tap(t, K_U);                                   /* a normal letter */
    tap(t, K_A);
    hold(t, K_LEFTSHIFT, true);                    /* Shift+Backspace */
    tap(t, K_BKSP);
    hold(t, K_LEFTSHIFT, false);
    hold(t, K_LEFTCTRL, true);                     /* Ctrl+Backspace */
    tap(t, K_BKSP);
    hold(t, K_LEFTCTRL, false);
    XWT_WAIT(t, g_nkrec >= 14);
    /* modifier release symmetry: no stray presses remain */
    hold(t, K_LEFTALT, true);                      /* Alt press/release */
    hold(t, K_LEFTALT, false);
    XWT_WAIT(t, g_nkrec >= 16);

    /* 4. assert the exact stream — keycode, keysym, press/release.
     * Ordering is asserted too: a stale/replayed/duplicated event
     * shifts the stream and fails here with the offending index. */
    check_krec(t, 0, K_BKSP, true, XKB_KEY_BackSpace, "Backspace press");
    check_krec(t, 1, K_BKSP, false, XKB_KEY_BackSpace, "Backspace release");
    check_krec(t, 2, K_U, true, XKB_KEY_u, "u press");
    check_krec(t, 3, K_U, false, XKB_KEY_u, "u release");
    check_krec(t, 4, K_A, true, XKB_KEY_a, "a press");
    check_krec(t, 5, K_A, false, XKB_KEY_a, "a release");
    check_krec(t, 6, K_LEFTSHIFT, true, XKB_KEY_Shift_L, "Shift press");
    check_krec(t, 7, K_BKSP, true, XKB_KEY_BackSpace, "Shift+Bksp press");
    check_krec(t, 8, K_BKSP, false, XKB_KEY_BackSpace, "Shift+Bksp release");
    check_krec(t, 9, K_LEFTSHIFT, false, XKB_KEY_Shift_L, "Shift release");
    check_krec(t, 10, K_LEFTCTRL, true, XKB_KEY_Control_L, "Ctrl press");
    check_krec(t, 11, K_BKSP, true, XKB_KEY_BackSpace, "Ctrl+Bksp press");
    check_krec(t, 12, K_BKSP, false, XKB_KEY_BackSpace, "Ctrl+Bksp release");
    check_krec(t, 13, K_LEFTCTRL, false, XKB_KEY_Control_L, "Ctrl release");
    check_krec(t, 14, K_LEFTALT, true, XKB_KEY_Alt_L, "Alt press");
    check_krec(t, 15, K_LEFTALT, false, XKB_KEY_Alt_L, "Alt release");

    /* modifiers as delivered alongside the key events: the Shift press
     * must already show in the Shift+Bksp event's mods, and nothing
     * may remain depressed after the matrix fully unwinds */
    XWT_CHECK(g_krec[7].mods != 0, "Shift+Bksp carried mods=0 (modifier "
                                   "state never reached the client)");
    XWT_CHECK(g_krec[15].mods == 0,
              "mods leaked after all modifiers released (%u)",
              g_krec[15].mods);

    /* exactly 16 events: no extras (stale, replayed, or synthesized) */
    XWT_CHECK(g_nkrec == 16, "client saw %d key events, expected 16 "
                             "(extras = stale/replayed events)",
              g_nkrec);

    xwc_win_destroy(win);
}

/* ------------------------------------------------------------- trace
 * XW_INPUT_TRACE / XW_GEOMETRY_TRACE must be purely observational:
 * enabling either must never change compositor or session behavior —
 * in particular shutdown/logout. Regression shape (the reported
 * physical-box failure): a compositor whose stderr goes to a pipe
 * nobody drains (redirected diagnostics, a stalled reader — any sink
 * that stops consuming) receives a storm of key + motion events, then
 * SIGTERM — exactly what xw-session's logout path sends. A blocking
 * diagnostic write wedges the event loop; the SIGTERM source lives IN
 * the loop, so the signal is never dispatched; the supervisor's 1s
 * grace expires and the SIGKILL tears the process down with the VT
 * still in graphics mode and the keyboard still RAW — "logout becomes
 * impossible". The control variant proves the storm itself is
 * harmless; every trace variant must shut down just as fast. */
struct trace_variant {
    const char *name;
    int input, geom;
};

static const struct trace_variant trace_variants[] = {
    {"no-trace", 0, 0},         /* control: same storm, no instruments */
    {"input-trace", 1, 0},      /* XW_INPUT_TRACE=1 only */
    {"geometry-trace", 0, 1},   /* XW_GEOMETRY_TRACE=1 only */
    {"both-traces", 1, 1},      /* XW_INPUT_TRACE=1 XW_GEOMETRY_TRACE=1 */
};

/* the child: fresh compositor + client, a focused window, then an
 * input storm until SIGTERM arrives. stderr is the write end of a
 * pipe the parent never reads. Exits 0 on a clean signal-driven
 * shutdown, 2 on setup failure. */
static void trace_child_run(int input, int geom) __attribute__((noreturn));
static void trace_child_run(int input, int geom) {
    if (input)
        setenv("XW_INPUT_TRACE", "1", 1);
    else
        unsetenv("XW_INPUT_TRACE");
    if (geom)
        setenv("XW_GEOMETRY_TRACE", "1", 1);
    else
        unsetenv("XW_GEOMETRY_TRACE");

    struct xwt_ctx t;
    if (xwt_begin(&t, NULL) < 0)
        _exit(2);

    struct xwc_win *win = xwt_window_solid(&t, 0xff40708a, 220, 160,
                                           "TraceStorm");
    if (!win)
        _exit(2);

    /* wait for the map + keyboard focus (bounded pump loop, no macro
     * noise on the shared stdout) */
    bool focused = false;
    for (int i = 0; i < 800 && !focused; i++) {
        focused = t.comp->wm->focused &&
                  strcmp(t.comp->wm->focused->title, "TraceStorm") == 0 &&
                  t.comp->wm->focused->surface != NULL;
        xwt_pump(&t);
    }
    if (!focused)
        _exit(2);

    struct xw_window *fw = t.comp->wm->focused;
    int wx = fw->x + fw->w / 4, wy = fw->y + fw->h / 3;

    /* xw_compositor_run() owns the running flag in production; the
     * white-box child drives the loop itself, so arm it here — the
     * SIGTERM source (on_signal) clears it, exactly like production */
    t.comp->running = true;

    /* the storm: keys through the full seat chain (entry + outcome
     * trace lines), three motions per iteration over the window (the
     * pointer-motion trace lines + the per-motion [geom] line — the
     * geometry instrument's hot-path volume). Three motions keep the
     * volume high enough that ANY instrument's line stream exceeds the
     * 64KB pipe capacity well before the parent's SIGTERM, so the
     * wedge/no-wedge outcome never depends on a timing race. */
    for (int i = 0; i < 3000; i++) {
        xw_compositor_inject_key(t.comp, 30, i & 1); /* KEY_A down/up */
        for (int m = 0; m < 3; m++)
            xw_compositor_inject_pointer_motion(
                t.comp, wx + ((i + m * 21) % 64), wy + ((i + m * 13) % 32));
        xw_compositor_dispatch(t.comp, 0);
        if ((i & 15) == 0)
            xwt_pump(&t); /* keep the client socket drained */
        if (!t.comp->running)
            break;
    }

    /* idle dispatch until the SIGTERM arrives (the supervisor's
     * stop_compositor path) */
    while (t.comp->running)
        xw_compositor_dispatch(t.comp, 20);

    xwt_end(&t);
    _exit(0);
}

static void test_trace_shutdown_observational(struct xwt_ctx *t) {
    (void)t; /* the parent runs no compositor work itself: children do */
    for (size_t v = 0; v < sizeof(trace_variants) / sizeof(trace_variants[0]);
         v++) {
        const struct trace_variant *tv = &trace_variants[v];
        int err_pipe[2];
        if (pipe(err_pipe) < 0) {
            XWT_CHECK(false, "pipe() failed: %s", strerror(errno));
            return;
        }
        pid_t pid = fork();
        if (pid < 0) {
            XWT_CHECK(false, "fork() failed: %s", strerror(errno));
            close(err_pipe[0]);
            close(err_pipe[1]);
            return;
        }
        if (pid == 0) {
            /* stderr -> a pipe the parent holds open but NEVER reads:
             * the stalled sink. stdout stays the suite's stdout; the
             * child prints nothing on the success path. */
            close(err_pipe[0]);
            dup2(err_pipe[1], STDERR_FILENO);
            if (err_pipe[1] != STDERR_FILENO)
                close(err_pipe[1]);
            trace_child_run(tv->input, tv->geom);
        }
        close(err_pipe[1]);

        /* setup + storm: long enough for any instrument's line stream
         * to exceed the pipe capacity (the wedge, pre-fix) or for the
         * storm to finish (post-fix) */
        usleep(800000);
        kill(pid, SIGTERM); /* the logout path's first move */

        int status = 0;
        bool exited = false;
        for (int i = 0; i < 250 && !exited; i++) { /* 2.5s budget */
            if (waitpid(pid, &status, WNOHANG) == pid) {
                exited = true;
                break;
            }
            usleep(10000);
        }
        if (!exited) {
            /* the supervisor's fallback: SIGKILL, the broken logout.
             * First: WHERE is the child stuck? /proc syscall + wchan
             * turn "wedged" into an attributable fact. */
            char path[64], sbuf[256];
            int fd;
            snprintf(path, sizeof(path), "/proc/%d/syscall", (int)pid);
            fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, sbuf, sizeof(sbuf) - 1);
                close(fd);
                if (n > 0) {
                    sbuf[n] = 0;
                    printf("    [wedge %s] syscall: %s", tv->name, sbuf);
                }
            }
            snprintf(path, sizeof(path), "/proc/%d/wchan", (int)pid);
            fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, sbuf, sizeof(sbuf) - 1);
                close(fd);
                if (n > 0) {
                    sbuf[n] = 0;
                    printf("    [wedge %s] wchan: %s\n", tv->name, sbuf);
                }
            }
            fflush(stdout);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            XWT_CHECK(false,
                      "variant '%s': SIGTERM was never dispatched — the "
                      "compositor is wedged inside a diagnostic write to a "
                      "stalled stderr sink (tracing changed shutdown "
                      "behavior)",
                      tv->name);
        } else {
            XWT_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      "variant '%s': child exit status %d (expected clean "
                      "exit 0)",
                      tv->name, status);
        }
        if (getenv("XWT_TRACE_DEBUG")) {
            char dbuf[4097];
            ssize_t dn;
            size_t total = 0;
            char tail[1200];
            size_t tlen = 0;
            while ((dn = read(err_pipe[0], dbuf, sizeof(dbuf) - 1)) > 0) {
                total += (size_t)dn;
                if (total == (size_t)dn) {
                    dbuf[dn] = 0;
                    printf("    [debug %s] first %zd bytes:\n%.900s",
                           tv->name, dn, dbuf);
                }
                size_t take = (size_t)dn < sizeof(tail) ? (size_t)dn
                                                        : sizeof(tail);
                const char *src = dbuf + dn - take;
                if (tlen + take > sizeof(tail)) {
                    size_t drop = tlen + take - sizeof(tail);
                    memmove(tail, tail + drop, tlen - drop);
                    tlen -= drop;
                }
                memcpy(tail + tlen, src, take);
                tlen += take;
            }
            if (tlen > 0)
                printf("    [debug %s] tail:\n%.*s\n", tv->name, (int)tlen,
                       tail);
            printf("    [debug %s] pipe backlog: %zu bytes\n", tv->name,
                   total);
        }
        close(err_pipe[0]);
    }
}

static void test_seatd_switch_session(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "ok", &pid, sock);
    XWT_ASSERT(s != NULL);
    XWT_CHECK(xw_seat_session_switch_vt(s, 3) == 0,
              "switch_session round-trips (the mock only answers a valid "
              "SWITCH_SESSION message)");
    XWT_CHECK(xw_seat_session_switch_vt(s, -1) < 0,
              "negative session numbers are rejected client-side");
    xw_seat_session_destroy(s);
    mock_wait(pid);
    close_mock_env(sock);
}

static void test_seatd_server_error(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "error", &pid, sock);
    XWT_CHECK(s == NULL, "SERVER_ERROR during the handshake fails the open");
    mock_wait(pid);
    close_mock_env(sock);
}

static void test_seatd_garbage(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "garbage", &pid, sock);
    XWT_CHECK(s == NULL, "out-of-sync garbage fails the handshake safely");
    mock_wait(pid);
    close_mock_env(sock);
}

static void test_seatd_close_early(struct xwt_ctx *t) {
    pid_t pid;
    char sock[108];
    struct xw_seat_session *s = open_mock_seat(t, "close-early", &pid, sock);
    XWT_CHECK(s == NULL, "a seat manager that hangs up fails the open");
    mock_wait(pid);
    close_mock_env(sock);
}

#ifdef XW_HAVE_LIBSEAT
static void test_libseat_cross_check(struct xwt_ctx *t) {
    /* the REAL libseat client (its seatd backend) against OUR mock
     * server: both clients must speak the same protocol, which
     * validates the mock (and with it every built-in-client test
     * above) against upstream behavior. */
    char sock[108];
    pid_t pid = mock_start("ok", sock, sizeof(sock));
    if (pid <= 0) {
        XWT_CHECK(false, "the mock seatd server did not start");
        return;
    }
    setenv("SEATD_SOCK", sock, 1);
    setenv("LIBSEAT_BACKEND", "seatd", 1); /* do not use logind/builtin */

    struct xw_seat_session *s =
        xw_seat_session_open(t->comp, XW_SEAT_PROVIDER_LIBSEAT, "seat-mock");
    XWT_CHECK(s != NULL, "libseat's seatd backend opens a seat on our mock");
    if (s) {
        XWT_CHECK(strcmp(xw_seat_session_name(s), "seat-mock") == 0,
                  "libseat reports the mock's seat name");
        int fd = -1;
        int dev_id = xw_seat_session_open_device(s, "/dev/null", &fd);
        XWT_CHECK(dev_id >= 0 && fd >= 0, "libseat opens a device");
        if (dev_id >= 0)
            XWT_CHECK(xw_seat_session_close_device(s, dev_id) == 0,
                      "libseat closes the device");
        if (fd >= 0)
            close(fd);
        XWT_CHECK(xw_seat_session_switch_vt(s, 2) == 0,
                  "libseat switches the session");
        xw_seat_session_destroy(s); /* CLOSE_SEAT through libseat */
    }
    unsetenv("LIBSEAT_BACKEND");
    int st = mock_wait(pid);
    XWT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "the mock completed the libseat session cleanly too");
    close_mock_env(sock);
}
#endif

static void test_direct_no_vt(struct xwt_ctx *t) {
    (void)t;
    /* Only meaningful where there is no VT (containers, CI). On a real
     * TTY the direct provider would take the terminal over — taking a
     * VT during a test run is not acceptable, so we probe first. */
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty >= 0) {
        struct vt_stat vts;
        bool is_vt = ioctl(tty, VT_GETSTATE, &vts) == 0;
        close(tty);
        if (is_vt) {
            XWT_SKIP("running on a real VT; direct provider covered by "
                     "the manual hardware checklist");
            return;
        }
    }
    struct xw_seat_session *s =
        xw_seat_session_open(t->comp, XW_SEAT_PROVIDER_DIRECT, "seat0");
    XWT_CHECK(s == NULL, "direct provider refuses a non-VT /dev/tty");
}

static void test_auto_all_unavailable(struct xwt_ctx *t) {
    /* every provider deliberately broken: the open must fail (never
     * silently fall back to root or to a fake seat) */
    setenv("SEATD_SOCK", "/nonexistent/seatd.sock", 1);
    setenv("LIBSEAT_BACKEND", "seatd", 1); /* libseat: seatd only -> fails */

    struct xw_seat_session *s =
        xw_seat_session_open(t->comp, XW_SEAT_PROVIDER_AUTO, "seat0");
    XWT_CHECK(s == NULL, "auto provider selection fails when nothing works");

    unsetenv("SEATD_SOCK");
    unsetenv("LIBSEAT_BACKEND");
}

__attribute__((constructor)) static void register_seat(void) {
    static const struct xwt_test tests[] = {
        {"seat-seatd-handshake", test_seatd_handshake},
        {"seat-seatd-open-device", test_seatd_open_device},
        {"seat-seatd-disable-lifecycle", test_seatd_disable_lifecycle},
        {"seat-seatd-disable-autoack", test_seatd_disable_autoack},
        {"seat-vt-switch-keys", test_seat_vt_switch_keys},
        {"seat-keyboard-matrix", test_seat_keyboard_matrix},
        {"trace-shutdown-observational", test_trace_shutdown_observational},
        {"seat-seatd-switch-session", test_seatd_switch_session},
        {"seat-seatd-server-error", test_seatd_server_error},
        {"seat-seatd-garbage", test_seatd_garbage},
        {"seat-seatd-close-early", test_seatd_close_early},
#ifdef XW_HAVE_LIBSEAT
        {"seat-libseat-cross-check", test_libseat_cross_check},
#endif
        {"seat-direct-no-vt", test_direct_no_vt},
        {"seat-auto-all-unavailable", test_auto_all_unavailable},
    };
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
