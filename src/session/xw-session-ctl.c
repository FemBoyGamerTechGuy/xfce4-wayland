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
            "       %s run -- COMMAND [ARGS...]\n"
            "\n"
            "Commands: status | ping | logout | restart | shutdown | reboot |\n"
            "          suspend | hibernate | exit-dialog | run -- CMD...\n"
            "Connects to the xw-session control socket ($XDG_RUNTIME_DIR/"
            "xw-session.sock by default).\n",
            prog, prog);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return usage(argv[0], 1);
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0)
        return usage(argv[0], 0);

    char run_buf[512] = "";
    if (strcmp(cmd, "run") == 0) {
        /* xw-session-ctl run -- sleep 5   →   "run sleep 5" */
        int start = 2;
        if (argc > 2 && strcmp(argv[2], "--") == 0)
            start = 3;
        size_t used = snprintf(run_buf, sizeof(run_buf), "run ");
        for (int i = start; i < argc && used + 1 < sizeof(run_buf); i++) {
            int n = snprintf(run_buf + used, sizeof(run_buf) - used, "%s%s",
                             i > start ? " " : "", argv[i]);
            if (n < 0)
                break;
            used += (size_t)n;
        }
        if (used <= 4) {
            fprintf(stderr, "run: missing command\n");
            return usage(argv[0], 1);
        }
        cmd = run_buf;
    } else {
        static const char *known[] = {"status",      "ping",  "logout",
                                      "restart",     "shutdown", "reboot",
                                      "suspend",     "hibernate",
                                      "exit-dialog"};
        bool ok = false;
        for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
            if (strcmp(cmd, known[i]) == 0)
                ok = true;
        if (!ok) {
            fprintf(stderr, "unknown command '%s'\n", cmd);
            return usage(argv[0], 1);
        }
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
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "cannot connect to %s: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }

    dprintf(fd, "%s\n", cmd);
    /* read the reply line(s) */
    char buf[512];
    ssize_t rn;
    while ((rn = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[rn] = 0;
        fputs(buf, stdout);
    }
    close(fd);
    return 0;
}
