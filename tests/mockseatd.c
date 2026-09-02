/* mockseatd — a minimal seatd-protocol server for process-level tests.
 *
 * Speaks the documented seatd 0.9 wire protocol (OPEN_SEAT handshake,
 * device open with SCM_RIGHTS, close, switch, disable-ack-enable,
 * close-seat) well enough for the real compositor and the real libseat
 * client to open a seat against it. Not a seat manager: it grants
 * every request and hands out /dev/null fds. Used to prove, in tests,
 * that the compositor acquires seats through the actual protocol —
 * physical DRM remains a hardware test (TESTING.md).
 *
 * Usage: mockseatd SOCKPATH [SECONDS]
 * Exits when the client disconnects (or the timeout expires).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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
    (void)!send(fd, &m, sizeof(m), MSG_NOSIGNAL);
    if (size)
        (void)!send(fd, payload, size, MSG_NOSIGNAL);
}

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
    (void)!sendmsg(fd, &mh, MSG_NOSIGNAL);
}

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
    while (got < m.size) {
        size_t want = m.size - got;
        if (want > cap)
            want = cap;
        ssize_t n = read(fd, (char *)payload + got, want);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return m.opcode;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s SOCKPATH [SECONDS]\n", argv[0]);
        return 2;
    }
    int timeout_s = argc > 2 ? atoi(argv[2]) : 30;

    unlink(argv[1]);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
        return 101;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return 102;
    if (listen(lfd, 4) < 0)
        return 103;
    signal(SIGPIPE, SIG_IGN);
    printf("listening\n");
    fflush(stdout);

    alarm((unsigned)timeout_s);

    int fd = accept(lfd, NULL, NULL);
    close(lfd);
    if (fd < 0)
        return 104;

    char payload[256];
    if (read_msg(fd, payload, sizeof(payload)) != CL_OPEN_SEAT)
        return 110;

    const char name[] = "seat-mock";
    uint16_t name_len = (uint16_t)sizeof(name);
    char open_payload[64];
    memcpy(open_payload, &name_len, 2);
    memcpy(open_payload + 2, name, sizeof(name));
    send_msg(fd, SV_SEAT_OPENED, open_payload, (uint16_t)(2 + sizeof(name)));
    printf("seat-opened seat-mock\n");
    fflush(stdout);

    for (;;) {
        int op = read_msg(fd, payload, sizeof(payload));
        if (op < 0)
            break;
        switch (op) {
        case CL_OPEN_DEVICE: {
            uint16_t plen;
            memcpy(&plen, payload, 2);
            char path[256] = {0};
            if (plen > 0 && plen <= sizeof(path) - 1)
                memcpy(path, payload + 2, plen - 1);
            printf("open-device %s\n", path);
            fflush(stdout);
            int dfd = open(path[0] ? path : "/dev/null", O_RDWR | O_CLOEXEC);
            if (dfd < 0)
                dfd = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (dfd < 0) {
                int32_t e = errno ? errno : EIO;
                send_msg(fd, SV_ERROR, &e, 4);
                break;
            }
            send_fd_msg(fd, dfd);
            close(dfd);
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
            printf("seat-closed\n");
            fflush(stdout);
            close(fd);
            return 0;
        default:
            return 120;
        }
    }
    close(fd);
    return 0;
}
