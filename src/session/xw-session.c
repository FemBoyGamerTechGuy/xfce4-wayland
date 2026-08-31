/* xw-session — session manager: compositor supervision, autostart,
 * control socket, power actions.
 *
 * Architecture (see ARCHITECTURE.md): one unprivileged process owning
 * the session lifecycle. It starts xw-compositor as a supervised child,
 * launches autostart applications (XDG .desktop entries filtered with
 * OnlyShowIn=XFCE, matching xfce4-session behavior), and exposes a
 * private unix line protocol on a control socket used by xw-exit and
 * xw-session-ctl. Power actions delegate to loginctl (logind/elogind)
 * as an unprivileged user call; if that is unavailable the request
 * fails with an honest error instead of pretending.
 *
 * No D-Bus, no GLib, no daemons of convenience: libc + fork/exec only.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTL_BACKLOG 8
#define MAX_AUTOSTART 64
#define COMPOSITOR_MAX_RESTARTS 5

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_child_event = 0;
static int g_exit_code = 0;

/* session state */
struct session {
    char runtime_dir[512];
    char ctl_path[600];
    char socket_name[256];    /* wayland display name */
    pid_t comp_pid;
    int comp_restarts;
    bool comp_ready;
    bool shutting_down;
    bool restarting;          /* re-exec instead of plain exit */
    struct {
        pid_t pid;
        char name[300];
    } autostart[MAX_AUTOSTART];
    int n_autostart;
};

static struct session S;

/* -------------------------------------------------------------- utils */

static void on_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM)
        g_terminate = 1;
    if (sig == SIGCHLD)
        g_child_event = 1;
}

static void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_msg(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[xw-session %s] ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void reap_all(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void stop_autostart_apps(void) {
    for (int i = 0; i < S.n_autostart; i++) {
        if (S.autostart[i].pid > 0) {
            log_msg("info", "stopping '%s' (pid %d)", S.autostart[i].name,
                    (int)S.autostart[i].pid);
            kill(S.autostart[i].pid, SIGTERM);
        }
    }
    /* children are reaped by the SIGCHLD loop during shutdown */
}

static void stop_compositor(void) {
    if (S.comp_pid > 0) {
        log_msg("info", "stopping compositor (pid %d)", (int)S.comp_pid);
        kill(S.comp_pid, SIGTERM);
        /* give it a moment to shut down cleanly, then force */
        for (int i = 0; i < 50; i++) {
            if (waitpid(S.comp_pid, NULL, WNOHANG) == S.comp_pid) {
                S.comp_pid = -1;
                return;
            }
            usleep(20000); /* 20ms x 50 = 1s */
        }
        kill(S.comp_pid, SIGKILL);
        waitpid(S.comp_pid, NULL, 0);
        S.comp_pid = -1;
    }
}

static void cleanup_sockets(void) {
    unlink(S.ctl_path);
    if (S.socket_name[0]) {
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", S.runtime_dir, S.socket_name);
        unlink(path);
    }
}

/* --------------------------------------------------------- compositor */

static const char *find_compositor(void) {
    const char *env = getenv("XW_COMPOSITOR");
    if (env && *env)
        return env;
    static char path[1024];
    /* sibling of this binary: ../xw-compositor (build/bin layout) */
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 32);
    if (n > 0) {
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(path) - (slash + 1 - path),
                     "xw-compositor");
            if (access(path, X_OK) == 0)
                return path;
            /* also try the source-tree layout build/bin → build/bin */
        }
    }
    if (access("/usr/local/bin/xw-compositor", X_OK) == 0)
        return "/usr/local/bin/xw-compositor";
    return "xw-compositor"; /* PATH fallback */
}

/* spawn the compositor; returns pid, or -1 */
static pid_t start_compositor(void) {
    int outpipe[2];
    if (pipe(outpipe) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]);
        close(outpipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* child: compositor writes its socket path to stdout */
        setenv("XDG_RUNTIME_DIR", S.runtime_dir, 1);
        dup2(outpipe[1], STDOUT_FILENO);
        close(outpipe[0]);
        close(outpipe[1]);
        const char *args[] = {find_compositor(), "-q", NULL};
        execvp(args[0], (char *const *)args);
        log_msg("error", "execvp(compositor) failed: %s", strerror(errno));
        _exit(127);
    }
    close(outpipe[1]);

    /* read the socket path line (with a timeout via poll) */
    char line[300] = {0};
    struct pollfd pfd = {.fd = outpipe[0], .events = POLLIN};
    int total = 0;
    while (poll(&pfd, 1, 5000) == 1 && total < (int)sizeof(line) - 1) {
        ssize_t n = read(outpipe[0], line + total, sizeof(line) - 1 - total);
        if (n <= 0)
            break;
        total += (int)n;
        if (strchr(line, '\n'))
            break;
    }
    close(outpipe[0]);

    char *nl = strchr(line, '\n');
    if (nl)
        *nl = 0;
    if (line[0] != '/') {
        log_msg("error", "compositor did not report a socket path");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    /* derive the wayland display name (basename) */
    const char *base = strrchr(line, '/');
    base = base ? base + 1 : line;
    snprintf(S.socket_name, sizeof(S.socket_name), "%s", base);
    S.comp_pid = pid;
    S.comp_ready = true;
    log_msg("info", "compositor ready: display %s (pid %d)", S.socket_name,
            (int)pid);
    return pid;
}

/* --------------------------------------------------------- autostart */

/* minimal .desktop field reader (no libxw dependency here) */
static char *desktop_field(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char line[1024];
    bool in_entry = false;
    size_t klen = strlen(key);
    char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            in_entry = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_entry)
            continue;
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *val = line + klen + 1;
            char *nl = strchr(val, '\n');
            if (nl)
                *nl = 0;
            result = strdup(val);
            break;
        }
    }
    fclose(f);
    return result;
}

static bool only_show_xfce(const char *only, const char *not_show) {
    /* xfce4-session runs entries with OnlyShowIn containing XFCE, and
     * skips entries with NotShowIn containing XFCE; entries with no
     * OnlyShowIn at all are also run (upstream behavior) */
    if (not_show && strstr(not_show, "XFCE;"))
        return false;
    if (only && *only)
        return strstr(only, "XFCE;") != NULL;
    return true;
}

static pid_t spawn_autostart_exec(const char *exec, const char *name) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", S.runtime_dir, 1);
        setenv("WAYLAND_DISPLAY", S.socket_name, 1);
        unsetenv("DISPLAY"); /* wayland-native session: no X by default */
        const char *sh = getenv("XW_SHELL") ? getenv("XW_SHELL") : "/bin/sh";
        const char *args[] = {sh, "-c", exec, NULL};
        execvp(args[0], (char *const *)args);
        log_msg("error", "autostart exec '%s' failed: %s", exec,
                strerror(errno));
        _exit(127);
    }
    log_msg("info", "autostart '%s' (pid %d)", name, (int)pid);
    return pid;
}

static void autostart_scan_dir(const char *dir, const char *override_dir) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && S.n_autostart < MAX_AUTOSTART) {
        if (de->d_name[0] == '.' || !strstr(de->d_name, ".desktop"))
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        /* user entry overrides the system one with the same name */
        if (override_dir) {
            char upath[1024];
            snprintf(upath, sizeof(upath), "%s/%s", override_dir, de->d_name);
            if (access(upath, R_OK) == 0)
                continue;
        }

        char *type = desktop_field(path, "Type");
        char *hidden = desktop_field(path, "Hidden");
        char *exec = desktop_field(path, "Exec");
        char *only = desktop_field(path, "OnlyShowIn");
        char *notshow = desktop_field(path, "NotShowIn");
        char *tryexec = desktop_field(path, "TryExec");

        if (type && strcmp(type, "Application") == 0 && exec &&
            !(hidden && strcmp(hidden, "true") == 0) &&
            (!tryexec || access(tryexec, X_OK) == 0) &&
            only_show_xfce(only, notshow)) {
            pid_t pid = spawn_autostart_exec(exec, de->d_name);
            if (pid > 0) {
                snprintf(S.autostart[S.n_autostart].name,
                         sizeof(S.autostart[0].name), "%s", de->d_name);
                S.autostart[S.n_autostart].pid = pid;
                S.n_autostart++;
            }
        }
        free(type);
        free(hidden);
        free(exec);
        free(only);
        free(notshow);
        free(tryexec);
    }
    closedir(d);
}

static void run_autostart(void) {
    const char *home = getenv("HOME");
    char user_dir[512] = {0};
    if (home)
        snprintf(user_dir, sizeof(user_dir), "%s/.config/autostart", home);
    /* the user directory overrides the system one */
    if (user_dir[0])
        autostart_scan_dir(user_dir, NULL);
    autostart_scan_dir("/etc/xdg/autostart", user_dir[0] ? user_dir : NULL);
}

/* ------------------------------------------------------------ control */

/* power management via loginctl (works with logind and elogind) */
static bool power_action(const char *verb) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "loginctl %s", verb);
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        log_msg("error",
                "power action '%s' failed (no logind/elogind or not "
                "permitted)",
                verb);
        return false;
    }
    return true;
}

static void session_shutdown(bool restart) {
    if (S.shutting_down)
        return;
    S.shutting_down = true;
    S.restarting = restart;
    log_msg("info", "session %s", restart ? "restarting" : "ending");
    stop_autostart_apps();
    stop_compositor();
    usleep(100000); /* let clients notice the compositor died */
    reap_all();
}

static void send_line(int fd, const char *line) {
    size_t len = strlen(line);
    (void)!write(fd, line, len);
    (void)!write(fd, "\n", 1);
}

static void handle_ctl_line(int fd, char *line) {
    /* strip newline */
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = 0;
    if (nl == line)
        return;

    if (strcmp(line, "status") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "ok compositor=%s pid=%d restarts=%d autostart=%d",
                 S.comp_ready ? "running" : "stopped",
                 S.comp_pid > 0 ? (int)S.comp_pid : 0, S.comp_restarts,
                 S.n_autostart);
        send_line(fd, buf);
        return;
    }
    if (strcmp(line, "logout") == 0) {
        send_line(fd, "ok ending session");
        session_shutdown(false);
        g_terminate = 1;
        g_exit_code = 0;
        return;
    }
    if (strcmp(line, "restart") == 0) {
        send_line(fd, "ok restarting session");
        session_shutdown(true);
        g_terminate = 1;
        return;
    }
    if (strcmp(line, "shutdown") == 0 || strcmp(line, "reboot") == 0 ||
        strcmp(line, "suspend") == 0 || strcmp(line, "hibernate") == 0) {
        if (strcmp(line, "shutdown") == 0 && power_action("poweroff")) {
            send_line(fd, "ok powering off");
            session_shutdown(false);
        } else if (strcmp(line, "reboot") == 0 && power_action("reboot")) {
            send_line(fd, "ok rebooting");
            session_shutdown(false);
        } else if (strcmp(line, "suspend") == 0 &&
                   power_action("suspend")) {
            send_line(fd, "ok suspending");
            /* suspend does not end the session */
        } else if (strcmp(line, "hibernate") == 0 &&
                   power_action("hibernate")) {
            send_line(fd, "ok hibernating");
        } else {
            send_line(fd,
                      "error power management unavailable (loginctl "
                      "failed)");
        }
        return;
    }
    if (strcmp(line, "ping") == 0) {
        send_line(fd, "ok pong");
        return;
    }
    send_line(fd, "error unknown command");
}

static int ctl_accept_loop(int listen_fd) {
    /* non-blocking accept in the main poll loop handled by caller; this
     * is the per-connection handler */
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) != 1)
        return 0;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return -1;
    /* line protocol: read until newline or EOF (with timeout) */
    char buf[512];
    int total = 0;
    for (;;) {
        struct pollfd rpfd = {.fd = fd, .events = POLLIN};
        if (poll(&rpfd, 1, 2000) != 1)
            break;
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += (int)n;
        buf[total] = 0;
        if (strchr(buf, '\n'))
            break;
        if (total >= (int)sizeof(buf) - 1)
            break;
    }
    if (total > 0)
        handle_ctl_line(fd, buf);
    close(fd);
    return 0;
}

static int ctl_listen(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(S.ctl_path) >= sizeof(addr.sun_path)) {
        log_msg("error", "control socket path too long");
        close(fd);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", S.ctl_path);
    unlink(S.ctl_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("error", "ctl bind %s: %s", S.ctl_path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(S.ctl_path, 0700);
    if (listen(fd, CTL_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    /* non-blocking so the poll loop can multiplex */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* ---------------------------------------------------------------- main */

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "The xfce4-wayland session manager.\n"
           "\n"
           "Options:\n"
           "  -S, --ctl-name NAME    control socket name (default: xw-session)\n"
           "  -n, --no-autostart     skip XDG autostart entries\n"
           "  -h, --help             this help\n",
           prog);
}

int main(int argc, char **argv) {
    const char *ctl_name = "xw-session";
    bool autostart = true;

    static const struct option longopts[] = {
        {"ctl-name", required_argument, NULL, 'S'},
        {"no-autostart", no_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "S:nh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'S':
            ctl_name = optarg;
            break;
        case 'n':
            autostart = false;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* runtime dir */
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    if (!rtd || !*rtd) {
        char fallback[256];
        snprintf(fallback, sizeof(fallback), "/run/user/%d", getuid());
        if (access(fallback, W_OK) == 0) {
            setenv("XDG_RUNTIME_DIR", fallback, 1);
            rtd = fallback;
        } else {
            fprintf(stderr, "[xw-session error] XDG_RUNTIME_DIR not set and "
                            "no writable /run/user/%d\n",
                    getuid());
            return 1;
        }
    }
    snprintf(S.runtime_dir, sizeof(S.runtime_dir), "%s", rtd);
    snprintf(S.ctl_path, sizeof(S.ctl_path), "%s/%s.sock", rtd, ctl_name);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* 1. compositor */
    if (start_compositor() < 0) {
        log_msg("error", "cannot start the compositor");
        return 1;
    }

    /* 2. control socket */
    int ctl_fd = ctl_listen();
    if (ctl_fd < 0)
        goto out;

    /* 3. autostart applications */
    if (autostart)
        run_autostart();

    /* 4. main loop: ctl requests + child supervision */
    log_msg("info", "session ready (ctl %s)", S.ctl_path);
    while (!g_terminate) {
        struct pollfd pfd = {.fd = ctl_fd, .events = POLLIN};
        int rc = poll(&pfd, 1, 500);
        if (rc == 1 && (pfd.revents & POLLIN))
            ctl_accept_loop(ctl_fd);

        if (g_child_event) {
            g_child_event = 0;
            /* compositor exit? */
            int status = 0;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            while (pid > 0) {
                if (pid == S.comp_pid) {
                    S.comp_pid = -1;
                    S.comp_ready = false;
                    if (S.shutting_down) {
                        /* expected during teardown */
                    } else if (S.comp_restarts < COMPOSITOR_MAX_RESTARTS) {
                        S.comp_restarts++;
                        log_msg("warn",
                                "compositor exited (status %d), restarting "
                                "(%d/%d)",
                                WEXITSTATUS(status), S.comp_restarts,
                                COMPOSITOR_MAX_RESTARTS);
                        usleep(300000);
                        if (start_compositor() < 0) {
                            log_msg("error", "compositor restart failed");
                            g_terminate = 1;
                            g_exit_code = 1;
                        }
                    } else {
                        log_msg("error",
                                "compositor kept crashing; giving up");
                        g_terminate = 1;
                        g_exit_code = 1;
                    }
                } else {
                    /* autostart app exited: bookkeeping only */
                    for (int i = 0; i < S.n_autostart; i++) {
                        if (S.autostart[i].pid == pid) {
                            S.autostart[i].pid = -1;
                            break;
                        }
                    }
                }
                pid = waitpid(-1, &status, WNOHANG);
            }
        }
    }

    close(ctl_fd);

out:
    session_shutdown(S.restarting);
    cleanup_sockets();
    log_msg("info", "session manager exiting");

    if (S.restarting) {
        /* re-exec ourselves for a fresh session */
        char flag[] = "-S";
        char name[128];
        snprintf(name, sizeof(name), "%s", ctl_name);
        char *args[] = {argv[0], flag, name, NULL};
        execvp(argv[0], args);
        /* if exec fails we fall through with an error */
        log_msg("error", "re-exec failed: %s", strerror(errno));
        return 1;
    }
    return g_exit_code;
}
