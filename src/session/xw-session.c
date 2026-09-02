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
#include <time.h>

#include "xw-power.h"
#include <sys/wait.h>
#include <unistd.h>

#define CTL_BACKLOG 8
#define MAX_AUTOSTART 64
#define MAX_RUNTIME_CHILDREN 16
#define COMPOSITOR_MAX_RESTARTS 5

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* human-readable wait status for exit logs */
static const char *wait_desc(int status) {
    static char buf[64];
    if (WIFEXITED(status))
        snprintf(buf, sizeof(buf), "exit status %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, sizeof(buf), "killed by signal %d", WTERMSIG(status));
    else
        snprintf(buf, sizeof(buf), "stopped (status 0x%x)", status);
    return buf;
}

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_child_event = 0;
static int g_exit_code = 0;

/* session state */
struct session {
    char runtime_dir[512];
    char ctl_path[600];
    char socket_name[256];    /* wayland display name */
    pid_t comp_pid;
    int64_t comp_started_ms;
    int comp_restarts;
    bool comp_ready;
    bool shutting_down;
    bool restarting;          /* re-exec instead of plain exit */
    bool nested;              /* desktop runs inside the parent session */
    bool verbose;             /* un-silence the compositor's diagnostics */
    bool want_panel;          /* launch the panel as a session component
                               * (like xfce4-session starts xfce4-panel) */
    /* backend selection (explicit --backend or auto):
     *   drm — real KMS session through a seat provider (never falls
     *         back; failure is fatal with diagnostics)
     *   x11 / wayland — nested under a parent session (implies nested)
     *   headless — in-memory outputs
     *   auto — TTY with KMS hardware -> drm; graphical parent or no
     *         KMS -> headless; -N -> parent-session window */
    int backend;              /* SB_* */
    bool fatal_backend;       /* explicit drm: a compositor exit is fatal,
                               * never a restart loop */
    char comp_backend[16];    /* resolved backend name passed to -B */
    struct {
        pid_t pid;
        int64_t started_ms;
        char name[300];
    } autostart[MAX_AUTOSTART];
    int n_autostart;
    /* children spawned at runtime via the ctl socket (exit dialog,
     * "run" commands): supervised like autostart — SIGTERM at
     * shutdown, reaped by the SIGCHLD loop */
    struct {
        pid_t pid;
        int64_t started_ms;
        char name[300];
    } spawned[MAX_RUNTIME_CHILDREN];
    int n_spawned;
};

static struct session S;

enum {
    SB_AUTO = 0,
    SB_DRM,
    SB_X11,
    SB_WAYLAND,
    SB_HEADLESS,
};

static void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* nested mode: run the desktop as a window inside the user's current
 * session (the primary development workflow before DRM/KMS). Backend
 * choice: explicit $XW_BACKEND, else a Wayland parent if
 * $WAYLAND_DISPLAY is set, else an X11 parent if $DISPLAY is set. */
static const char *nested_backend_arg(void) {
    const char *forced = getenv("XW_BACKEND");
    if (forced && *forced)
        return forced;
    const char *wl = getenv("WAYLAND_DISPLAY");
    if (wl && *wl)
        return "nested";
    const char *x = getenv("DISPLAY");
    if (x && *x)
        return "x11";
    return NULL;
}

/* Is KMS display hardware reachable in this session? Used by AUTO
 * only: an explicit --backend=drm must NOT depend on this (its
 * failures are the compositor's honest diagnostics, not a silent
 * downgrade). */
static bool kms_hardware_present(void) {
    DIR *d = opendir("/dev/dri");
    if (!d)
        return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "card", 4) == 0 && de->d_name[4] >= '0' &&
            de->d_name[4] <= '9') {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Resolve the effective backend + the -B argument for the compositor.
 * Returns the backend name ("drm", "x11", "nested", "headless") or
 * NULL when nesting was requested but no parent session exists. */
static const char *resolve_backend(bool *fatal_on_fail) {
    *fatal_on_fail = false;

    if (S.backend == SB_DRM) {
        *fatal_on_fail = true; /* explicit: never silently downgraded */
        return "drm";
    }
    if (S.backend == SB_X11) {
        S.nested = true;
        return "x11";
    }
    if (S.backend == SB_WAYLAND) {
        S.nested = true;
        return "nested";
    }
    if (S.backend == SB_HEADLESS)
        return "headless";

    /* AUTO */
    if (S.nested) {
        const char *be = nested_backend_arg();
        if (!be) {
            log_msg("error", "nested mode requires $WAYLAND_DISPLAY or "
                            "$DISPLAY (or set $XW_BACKEND)");
            return NULL;
        }
        return be;
    }
    /* a real TTY with KMS hardware is a real session: drive the display */
    const char *wl = getenv("WAYLAND_DISPLAY");
    const char *x = getenv("DISPLAY");
    if ((!wl || !*wl) && (!x || !*x)) {
        if (kms_hardware_present()) {
            log_msg("info", "auto: TTY login with KMS hardware — starting "
                            "the real DRM session (seat provider: auto)");
            return "drm";
        }
        log_msg("info", "auto: TTY login, but no /dev/dri KMS hardware — "
                        "starting headless (no display will appear)");
    }
    /* inside a graphical parent session (or headless CI): the default
     * remains headless; nesting is an explicit -N */
    return "headless";
}

/* -------------------------------------------------------------- utils */

static void on_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM)
        g_terminate = 1;
    if (sig == SIGCHLD)
        g_child_event = 1;
}

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
    for (int i = 0; i < S.n_spawned; i++) {
        if (S.spawned[i].pid > 0) {
            log_msg("info", "stopping '%s' (pid %d)", S.spawned[i].name,
                    (int)S.spawned[i].pid);
            kill(S.spawned[i].pid, SIGTERM);
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

/* user configuration directory: $XDG_CONFIG_HOME/xfce4-wayland
 * (created on first run so the INI files short-cuts and rules live in
 * actually take effect inside the session) */
static const char *user_config_dir(void) {
    static char dir[512];
    const char *base = getenv("XDG_CONFIG_HOME");
    if (!base || !*base) {
        static char home[448];
        const char *h = getenv("HOME");
        if (!h || !*h)
            return NULL;
        snprintf(home, sizeof(home), "%s/.config", h);
        base = home;
    }
    snprintf(dir, sizeof(dir), "%s/xfce4-wayland", base);
    mkdir(dir, 0700); /* fine if it exists */
    return dir;
}

/* spawn the compositor; returns pid, or -1 */
static pid_t start_compositor(void) {
    int outpipe[2];
    if (pipe(outpipe) < 0)
        return -1;

    bool fatal_backend = false;
    const char *backend = resolve_backend(&fatal_backend);
    if (!backend)
        return -1;
    S.fatal_backend = fatal_backend;
    snprintf(S.comp_backend, sizeof(S.comp_backend), "%s", backend);

    /* build the argument list: -q (unless verbose), the user config
     * dir, the backend, the seat provider env is inherited */
    const char *args[16] = {0};
    int nargs = 0;
    args[nargs++] = find_compositor();
    args[nargs++] = S.verbose ? "-v" : "-q";
    const char *conf = user_config_dir();
    if (conf) {
        args[nargs++] = "--config-dir";
        args[nargs++] = conf;
    }
    args[nargs++] = "--backend";
    args[nargs++] = backend;
    log_msg("info", "compositor backend: %s", backend);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        /* child: compositor writes its socket path to stdout */
        setenv("XDG_RUNTIME_DIR", S.runtime_dir, 1);
        if (strcmp(backend, "drm") == 0) {
            /* a real session: the Wayland environment children expect;
             * no X display leaks into the session */
            setenv("XDG_SESSION_TYPE", "wayland", 1);
            setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
            setenv("XDG_SESSION_DESKTOP", "xfce", 1);
            unsetenv("DISPLAY");
        }
        dup2(outpipe[1], STDOUT_FILENO);
        close(outpipe[0]);
        close(outpipe[1]);
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
    if (snprintf(S.socket_name, sizeof(S.socket_name), "%s", base) >=
        (int)sizeof(S.socket_name)) {
        log_msg("error", "socket name too long");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    S.comp_pid = pid;
    S.comp_started_ms = now_ms();
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

/* duplicate-service detection: is a process with this command's
 * basename already running under this uid? Covers the re-login and
 * session-restart flows, where blindly re-spawning xfsettingsd /
 * polkit agents / power managers would fail ("already running") and
 * look like a compositor crash. Best-effort by design. */
static bool already_running(const char *exec) {
    /* first token of the command line that is not an env assignment */
    char first[256] = {0};
    const char *p = exec;
    while (*p == ' ')
        p++;
    for (;;) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        if (len >= sizeof(first))
            len = sizeof(first) - 1;
        memcpy(first, p, len);
        first[len] = 0;
        /* skip leading env assignments (VAR=value cmd) */
        if (!strchr(first, '=') && strcmp(first, "env") != 0)
            break;
        if (!sp)
            return false; /* only assignments? nothing to check */
        p = sp + 1;
        while (*p == ' ')
            p++;
    }
    const char *base = strrchr(first, '/');
    base = base ? base + 1 : first;
    if (!*base)
        return false;

    DIR *d = opendir("/proc");
    if (!d)
        return false;
    struct dirent *de;
    pid_t me = getpid();
    while ((de = readdir(d))) {
        char *end;
        long pid = strtol(de->d_name, &end, 10);
        if (*end || pid <= 0 || (pid_t)pid == me)
            continue;
        char path[64], cmd[512];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        cmd[n] = 0;
        /* cmdline args are NUL-separated: check each token's basename */
        for (size_t i = 0; i < n;) {
            char *tok = cmd + i;
            size_t tlen = strlen(tok);
            i += tlen + 1;
            const char *tb = strrchr(tok, '/');
            tb = tb ? tb + 1 : tok;
            if (strcmp(tb, base) == 0) {
                closedir(d);
                return true;
            }
        }
    }
    closedir(d);
    return false;
}

static pid_t spawn_autostart_exec(const char *exec, const char *name) {
    if (already_running(exec)) {
        log_msg("info", "autostart '%s': already running elsewhere — "
                        "skipping (duplicate service ownership is not an "
                        "error)", name);
        return -2; /* not an error, not a child */
    }
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", S.runtime_dir, 1);
        setenv("WAYLAND_DISPLAY", S.socket_name, 1);
        if (strcmp(S.comp_backend, "drm") == 0) {
            setenv("XDG_SESSION_TYPE", "wayland", 1);
            setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
            setenv("XDG_SESSION_DESKTOP", "xfce", 1);
            unsetenv("DISPLAY"); /* wayland-native session: no X */
        } else if (!S.nested) {
            unsetenv("DISPLAY"); /* headless dev session: no X either */
        } /* nested mode keeps it (XWayland future) */
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
                S.autostart[S.n_autostart].started_ms = now_ms();
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

/* --------------------------------------------------------- runtime spawns */

static pid_t spawn_runtime(const char *exec, const char *name);

/* PATH lookup for a session component (no shell): fills out[0] with the
 * first executable directory hit. Returns false when not found. */
static bool search_path(const char *name, char *out, size_t outn) {
    const char *path = getenv("PATH");
    if (!path || !*path)
        return false;
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < outn - strlen(name) - 2) {
            snprintf(out, outn, "%.*s/%s", (int)len, p, name);
            if (access(out, X_OK) == 0)
                return true;
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

/* The panel is a first-class session component, not an autostart app:
 * xfce4-session starts xfce4-panel itself, and a build-tree session
 * (no system install, no ~/.config/autostart) must still have a panel.
 * Resolution order: $XW_PANEL_CMD ("none" disables), the binary next
 * to this session manager (build/bin layout), /usr/local/bin, PATH.
 * Returns NULL (with a logged reason) when no panel exists at all. */
static const char *find_panel(void) {
    const char *env = getenv("XW_PANEL_CMD");
    if (env && *env) {
        if (strcmp(env, "none") == 0 || strcmp(env, "off") == 0) {
            log_msg("info", "panel: disabled via $XW_PANEL_CMD");
            return NULL;
        }
        return env;
    }
    static char path[1024];
    /* sibling of this binary: <dir>/xw-panel (build/bin layout) */
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 32);
    if (n > 0) {
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(path) - (slash + 1 - path), "xw-panel");
            if (access(path, X_OK) == 0)
                return path;
        }
    }
    if (access("/usr/local/bin/xw-panel", X_OK) == 0)
        return "/usr/local/bin/xw-panel";
    if (search_path("xw-panel", path, sizeof(path)))
        return path;
    log_msg("info", "panel: no xw-panel binary found next to the session "
                    "manager, in /usr/local/bin or on PATH — starting "
                    "without one (build it, or set $XW_PANEL_CMD)");
    return NULL;
}

/* Start the panel unless disabled, already running (re-login/restart
 * flows — same duplicate rule as autostart) or spawnable-free. Runs
 * after the compositor is ready; user autostart entries for xw-panel
 * are then skipped as duplicates, so exactly one panel exists. */
static void start_panel(void) {
    if (!S.want_panel) {
        log_msg("info", "panel: skipped (--no-panel)");
        return;
    }
    const char *cmd = find_panel();
    if (!cmd)
        return;
    if (already_running(cmd)) {
        log_msg("info", "panel: already running — reusing it");
        return;
    }
    /* --verbose lights the panel's startup-chain trace too: the child
     * copies the environment at fork (inside spawn_runtime), so the
     * parent unsets it right after to keep autostart apps clean */
    if (S.verbose)
        setenv("XW_PANEL_TRACE", "1", 1);
    pid_t pid = spawn_runtime(cmd, "xw-panel");
    if (S.verbose)
        unsetenv("XW_PANEL_TRACE");
    if (pid > 0)
        log_msg("info", "panel: started as a session component (pid %d)",
                (int)pid);
}

/* fork + exec via /bin/sh -c, tracked as a supervised session child
 * (same environment as autostart).  Returns the pid or -1. */
static pid_t spawn_runtime(const char *exec, const char *name) {
    if (S.n_spawned >= MAX_RUNTIME_CHILDREN) {
        log_msg("error", "runtime child table full");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", S.runtime_dir, 1);
        setenv("WAYLAND_DISPLAY", S.socket_name, 1);
        if (strcmp(S.comp_backend, "drm") == 0) {
            setenv("XDG_SESSION_TYPE", "wayland", 1);
            setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
            setenv("XDG_SESSION_DESKTOP", "xfce", 1);
            unsetenv("DISPLAY");
        }
        const char *sh = getenv("XW_SHELL") ? getenv("XW_SHELL") : "/bin/sh";
        const char *args[] = {sh, "-c", exec, NULL};
        execvp(args[0], (char *const *)args);
        log_msg("error", "runtime exec '%s' failed: %s", exec,
                strerror(errno));
        _exit(127);
    }
    snprintf(S.spawned[S.n_spawned].name,
             sizeof(S.spawned[0].name), "%s", name);
    S.spawned[S.n_spawned].pid = pid;
    S.spawned[S.n_spawned].started_ms = now_ms();
    S.n_spawned++;
    log_msg("info", "spawned '%s' (pid %d)", name, (int)pid);
    return pid;
}

/* the exit dialog: same binary the compositor's XW_ACTION_EXIT_DIALOG
 * spawns (actions.conf [commands] exit-dialog, default "xw-exit");
 * $XW_EXIT_CMD overrides for testing/development */
static const char *exit_dialog_command(void) {
    const char *env = getenv("XW_EXIT_CMD");
    if (env && *env)
        return env;
    return "xw-exit";
}

/* ------------------------------------------------------------ control */

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
    if (strcmp(line, "power-status") == 0) {
        struct xw_power_caps caps;
        xw_power_probe(&caps);
        char buf[768];
        snprintf(buf, sizeof(buf),
                 "ok loginctl=%s suspend=%s%s hibernate=%s%s poweroff=%s%s "
                 "reboot=%s%s",
                 caps.loginctl_ok ? "yes" : "no",
                 caps.suspend ? "yes" : "no:",
                 caps.suspend ? "" : caps.suspend_reason,
                 caps.hibernate ? "yes" : "no:",
                 caps.hibernate ? "" : caps.hibernate_reason,
                 caps.poweroff ? "yes" : "no:",
                 caps.poweroff ? "" : caps.poweroff_reason,
                 caps.reboot ? "yes" : "no:",
                 caps.reboot ? "" : caps.reboot_reason);
        /* one line: fold reasons to '_' for the line protocol */
        for (char *p = buf; *p; p++)
            if (*p == ' ')
                *p = '_';
        send_line(fd, buf);
        return;
    }
    if (strcmp(line, "shutdown") == 0 || strcmp(line, "reboot") == 0 ||
        strcmp(line, "suspend") == 0 || strcmp(line, "hibernate") == 0) {
        char err[192] = {0};
        if (strcmp(line, "shutdown") == 0 &&
            xw_power_exec("poweroff", err, sizeof(err))) {
            send_line(fd, "ok powering off");
            session_shutdown(false);
        } else if (strcmp(line, "reboot") == 0 &&
                   xw_power_exec("reboot", err, sizeof(err))) {
            send_line(fd, "ok rebooting");
            session_shutdown(false);
        } else if (strcmp(line, "suspend") == 0 &&
                   xw_power_exec("suspend", err, sizeof(err))) {
            send_line(fd, "ok suspending");
            /* suspend does not end the session */
        } else if (strcmp(line, "hibernate") == 0 &&
                   xw_power_exec("hibernate", err, sizeof(err))) {
            send_line(fd, "ok hibernating");
        } else {
            char reply[256];
            snprintf(reply, sizeof(reply),
                     "error power management unavailable%s%s",
                     err[0] ? ": " : "", err);
            send_line(fd, reply);
        }
        return;
    }
    if (strcmp(line, "exit-dialog") == 0) {
        /* same effect as the compositor's XW_ACTION_EXIT_DIALOG
         * (Ctrl+Alt+Del): the graphical session-exit dialog */
        if (!S.comp_ready) {
            send_line(fd, "error no compositor");
            return;
        }
        pid_t pid = spawn_runtime(exit_dialog_command(), "xw-exit");
        if (pid > 0)
            send_line(fd, "ok exit dialog spawned");
        else
            send_line(fd, "error cannot spawn the exit dialog");
        return;
    }
    if (strncmp(line, "run ", 4) == 0) {
        /* session-scoped command execution (panel launcher et al.):
         * the socket is 0700 and single-user, so this is no more
         * privileged than the user's own shell */
        const char *cmd = line + 4;
        while (*cmd == ' ')
            cmd++;
        if (!*cmd) {
            send_line(fd, "error empty command");
            return;
        }
        pid_t pid = spawn_runtime(cmd, cmd);
        if (pid > 0)
            send_line(fd, "ok spawned");
        else
            send_line(fd, "error cannot spawn");
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
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", S.ctl_path);
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        log_msg("error", "control socket path too long");
        close(fd);
        return -1;
    }
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
           "Backend selection:\n"
           "  auto     TTY with KMS hardware -> drm; otherwise headless.\n"
           "           A graphical parent needs -N/--nested or an explicit\n"
           "           --backend to become a window (the default inside a\n"
           "           parent session stays headless)\n"
           "  drm      real display hardware through a seat provider\n"
           "           (libseat/seatd/direct; never silently downgraded)\n"
           "  x11      nested window under an X11 session\n"
           "  wayland  nested window under a Wayland session\n"
           "  headless no display hardware\n"
           "\n"
           "Options:\n"
           "  -B, --backend NAME    drm | x11 | wayland | nested | headless\n"
           "                        (nested is an alias for wayland)\n"
           "  -N, --nested          run the desktop inside the current session\n"
           "                        (window under Wayland or X11; $XW_BACKEND\n"
           "                        overrides the auto-choice)\n"
           "  -S, --ctl-name NAME   control socket name (default: xw-session)\n"
           "  -n, --no-autostart    skip XDG autostart entries\n"
           "  -p, --no-panel        skip the session panel (it is a session\n"
           "                        component like xfce4-panel, started even\n"
           "                        without autostart entries; $XW_PANEL_CMD=none\n"
           "                        does the same)\n"
           "  -V, --verbose         show seat/backend diagnostics from the\n"
           "                        compositor (seat provider, DRM device,\n"
           "                        connector, mode, socket, input devices)\n"
           "  -h, --help            this help\n",
           prog);
}

int main(int argc, char **argv) {
    const char *ctl_name = "xw-session";
    bool autostart = true;
    S.nested = false;
    S.verbose = false;
    S.want_panel = true;
    S.backend = SB_AUTO;

    static const struct option longopts[] = {
        {"ctl-name", required_argument, NULL, 'S'},
        {"no-autostart", no_argument, NULL, 'n'},
        {"no-panel", no_argument, NULL, 'p'},
        {"nested", no_argument, NULL, 'N'},
        {"backend", required_argument, NULL, 'B'},
        {"verbose", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "S:npB:Vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'S':
            ctl_name = optarg;
            break;
        case 'n':
            autostart = false;
            break;
        case 'p':
            S.want_panel = false;
            break;
        case 'N':
            S.nested = true;
            break;
        case 'B': {
            if (strcmp(optarg, "drm") == 0)
                S.backend = SB_DRM;
            else if (strcmp(optarg, "x11") == 0)
                S.backend = SB_X11;
            else if (strcmp(optarg, "wayland") == 0 ||
                     strcmp(optarg, "nested") == 0)
                S.backend = SB_WAYLAND;
            else if (strcmp(optarg, "headless") == 0)
                S.backend = SB_HEADLESS;
            else if (strcmp(optarg, "auto") == 0)
                S.backend = SB_AUTO;
            else {
                fprintf(stderr,
                        "unknown backend '%s' (drm|x11|wayland|headless)\n",
                        optarg);
                return 1;
            }
            break;
        }
        case 'V':
            S.verbose = true;
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

    /* 3. the panel: a session component (like xfce4-panel inside
     * xfce4-session), not an autostart app — a build-tree session
     * with no ~/.config/autostart still gets a panel */
    start_panel();

    /* 4. autostart applications */
    if (autostart)
        run_autostart();

    /* 5. main loop: ctl requests + child supervision */
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
                    int64_t secs =
                        (now_ms() - S.comp_started_ms + 500) / 1000;
                    if (S.shutting_down) {
                        /* expected during teardown */
                        log_msg("info", "compositor exited during shutdown "
                                        "(%s, ran %llds)",
                                wait_desc(status), (long long)secs);
                    } else if (S.comp_restarts < COMPOSITOR_MAX_RESTARTS &&
                               !S.fatal_backend) {
                        S.comp_restarts++;
                        log_msg("warn",
                                "compositor exited (%s, ran %llds), "
                                "restarting (%d/%d)",
                                wait_desc(status), (long long)secs,
                                S.comp_restarts, COMPOSITOR_MAX_RESTARTS);
                        usleep(300000);
                        if (start_compositor() < 0) {
                            log_msg("error", "compositor restart failed");
                            g_terminate = 1;
                            g_exit_code = 1;
                        }
                    } else if (S.fatal_backend) {
                        log_msg("error",
                                "compositor with the explicit '%s' backend "
                                "exited (%s, ran %llds) — the session ends "
                                "instead of restarting or falling back (see "
                                "the compositor diagnostics above)",
                                S.comp_backend, wait_desc(status),
                                (long long)secs);
                        g_terminate = 1;
                        g_exit_code = 1;
                    } else {
                        log_msg("error",
                                "compositor kept crashing; giving up");
                        g_terminate = 1;
                        g_exit_code = 1;
                    }
                } else {
                    /* autostart or runtime-spawned child exited: report
                     * it — a silently dead panel must never look like a
                     * working one (an autostart that dies instantly is
                     * almost always a bad Exec= line or a missing
                     * runtime dependency) */
                    const char *kind = NULL, *name = NULL;
                    int64_t started = 0;
                    for (int i = 0; i < S.n_autostart; i++) {
                        if (S.autostart[i].pid == pid) {
                            kind = "autostart";
                            name = S.autostart[i].name;
                            started = S.autostart[i].started_ms;
                            S.autostart[i].pid = -1;
                            break;
                        }
                    }
                    for (int i = 0; !kind && i < S.n_spawned; i++) {
                        if (S.spawned[i].pid == pid) {
                            kind = "spawned";
                            name = S.spawned[i].name;
                            started = S.spawned[i].started_ms;
                            S.spawned[i].pid = -1;
                            break;
                        }
                    }
                    if (kind) {
                        int64_t secs = (now_ms() - started + 500) / 1000;
                        log_msg("warn", "%s '%s' (pid %d) exited: %s "
                                        "[ran %llds]",
                                kind, name, (int)pid, wait_desc(status),
                                (long long)secs);
                        if (WIFEXITED(status) &&
                            WEXITSTATUS(status) == 127)
                            log_msg("error",
                                    "  pid %d could not execute its command "
                                    "(127): check the Exec= line of '%s' — "
                                    "use an absolute path or extend PATH",
                                    (int)pid, name);
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
        /* re-exec with the ORIGINAL argv: a restarted session keeps
         * every flag the user chose (ctl name, no-autostart, nested).
         * Rebuilding a minimal argv here silently dropped flags. */
        execvp(argv[0], argv);
        /* if exec fails we fall through with an error */
        log_msg("error", "re-exec failed: %s", strerror(errno));
        return 1;
    }
    return g_exit_code;
}
