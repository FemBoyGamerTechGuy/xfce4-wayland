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
