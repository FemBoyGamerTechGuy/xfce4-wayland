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
            "Usage: %s [-S NAME] COMMAND\n"
            "       %s [-S NAME] run -- COMMAND [ARGS...]\n"
            "\n"
            "Commands: status | ping | logout | restart | shutdown | reboot |\n"
            "          suspend | hibernate | exit-dialog | run -- CMD...\n"
            "Connects to the session control socket ($XDG_RUNTIME_DIR/"
            "NAME.sock; default NAME=xw-session, matching xw-session -S).\n",
            prog, prog);
    return rc;
}

int main(int argc, char **argv) {
    const char *ctl_name = "xw-session";
    int first = 1;
    if (argc >= 4 && (strcmp(argv[1], "-S") == 0 ||
                      strcmp(argv[1], "--ctl-name") == 0)) {
        ctl_name = argv[2];
        first = 3;
    }
    if (argc <= first)
        return usage(argv[0], 1);
    const char *cmd = argv[first];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0)
        return usage(argv[0], 0);

    char run_buf[512] = "";
    if (strcmp(cmd, "run") == 0) {
        /* xw-session-ctl run -- sleep 5   →   "run sleep 5" */
        int start = first + 1;
        if (argc > start && strcmp(argv[start], "--") == 0)
            start++;
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
    if (snprintf(path, sizeof(path), "%s/%s.sock", rtd, ctl_name) >=
        (int)sizeof(path)) {
        fprintf(stderr, "socket path too long\n");
        return 1;
    }

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
