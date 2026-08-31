/* xw-session-ctl — command-line client for the session control socket. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int usage(const char *prog, int rc) {
    fprintf(rc ? stderr : stdout,
            "Usage: %s COMMAND\n"
            "\n"
            "Commands: status | ping | logout | restart | shutdown | reboot |\n"
            "          suspend | hibernate\n"
            "Connects to the xw-session control socket ($XDG_RUNTIME_DIR/"
            "xw-session.sock by default).\n",
            prog);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return usage(argv[0], 1);
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0)
        return usage(argv[0], 0);

    static const char *known[] = {"status", "ping",  "logout", "restart",
                                  "shutdown", "reboot", "suspend", "hibernate"};
    bool ok = false;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(cmd, known[i]) == 0)
            ok = true;
    if (!ok) {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        return usage(argv[0], 1);
    }

    const char *rtd = getenv("XDG_RUNTIME_DIR");
    if (!rtd || !*rtd) {
        fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
        return 1;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/xw-session.sock", rtd);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "cannot connect to %s: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }

    dprintf(fd, "%s\n", cmd);
    /* read the reply line(s) */
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        fputs(buf, stdout);
    }
    close(fd);
    return 0;
}
