/* xw-ctl.c — see xw-ctl.h.  Extracted verbatim from xw-exit.c when the
 * panel needed the same wire (M8); behavior is identical: connect,
 * write "cmd\n", read one buffer, first line is the reply. */
#include "xw-ctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

bool xw_ctl_send(const char *cmd, char *reply, size_t reply_len) {
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    if (!rtd || !*rtd) {
        snprintf(reply, reply_len, "XDG_RUNTIME_DIR not set");
        return false;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/xw-session.sock", rtd);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(reply, reply_len, "socket: %s", strerror(errno));
        return false;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path) ||
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) < 0) {
        close(fd);
        snprintf(reply, reply_len, "path too long");
        return false;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(reply, reply_len, "no session manager (%s)", strerror(errno));
        close(fd);
        return false;
    }
    dprintf(fd, "%s\n", cmd);
    ssize_t n = read(fd, reply, reply_len - 1);
    close(fd);
    if (n <= 0) {
        snprintf(reply, reply_len, "no reply");
        return false;
    }
    reply[n] = 0;
    char *nl = strchr(reply, '\n');
    if (nl)
        *nl = 0;
    return strncmp(reply, "ok", 2) == 0;
}
