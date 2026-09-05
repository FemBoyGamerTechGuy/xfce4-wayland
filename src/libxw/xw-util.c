/* xw-util.c — logging, time, command helpers, INI parser. */
#include "xw-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- logging */
static int g_log_level = XW_LOG_INFO;
static void (*g_log_fn)(int level, const char *msg, void *ud) = NULL;
static void *g_log_ud = NULL;

void xw_log_set_level(int level) { g_log_level = level; }

void xw_log_set_callback(void (*fn)(int level, const char *msg, void *ud),
                         void *ud) {
    g_log_fn = fn;
    g_log_ud = ud;
}

static const char *level_name(int level) {
    switch (level) {
    case XW_LOG_DEBUG: return "debug";
    case XW_LOG_INFO:  return "info";
    case XW_LOG_WARN:  return "warn";
    default:           return "error";
    }
}

/* --------------------------------------------- the observational sink
 * Every line-level diagnostic (the XW_INPUT_TRACE / XW_GEOMETRY_TRACE
 * instruments and the default log sink) funnels through here. The
 * contract: emitting a line must NEVER block the caller for
 * unbounded time, whatever stderr is connected to — a pipe nobody
 * drains (redirected diagnostics, a stalled reader), a slow console,
 * an ssh connection that went away. The reason is not cosmetic: the
 * event loop owns the signal sources (signalfd), so a compositor
 * wedged inside a diagnostic write NEVER dispatches SIGTERM; the
 * session supervisor's 1s grace expires, the SIGKILL fallback tears
 * the process down with the VT still in graphics mode and the
 * keyboard still RAW — "logout becomes impossible". Observed on the
 * physical DRM box as: logout fine without XW_*_TRACE, dead with it.
 *
 * Mechanism: lines that cannot be written NOW are dropped and
 * counted; the count is reported on the next successful write. Sink
 * selection keeps every other writer (and every child process)
 * untouched:
 *   - regular files never block and must share fd 2's offset (the
 *     compositor binary's own fprintf would otherwise interleave
 *     destructively with an O_APPEND description) -> raw fd 2;
 *   - pipes and terminals get a PRIVATE reopen through
 *     /proc/self/fd/2 (a new file description) with O_NONBLOCK, so
 *     neither fd 2 nor any inherited descriptor changes semantics;
 *   - sockets cannot be reopened through /proc (open fails) -> fd 2
 *     guarded by a zero-timeout poll, accepting only the microscopic
 *     race of another process sharing the pipe between poll and
 *     write; lines stay atomic (single write, well under PIPE_BUF).
 */
static long g_diag_dropped;

static int diag_sink_fd(void) {
    static int fd = -2; /* -2 = unresolved, -1 = use fd 2 (guarded) */
    if (fd != -2)
        return fd;
    fd = -1;
    struct stat st;
    if (fstat(STDERR_FILENO, &st) == 0 && S_ISREG(st.st_mode)) {
        /* regular file: never blocks; must share offset with stdio
         * writers on fd 2 — use fd 2 directly */
        return fd = -1;
    }
    int f = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_APPEND |
                                       O_CLOEXEC);
    if (f >= 0)
        fd = f;
    return fd;
}

static void diag_write_raw(const char *buf, size_t len) {
    if (len == 0)
        return;
    int fd = diag_sink_fd();
    if (fd >= 0) {
        ssize_t w = write(fd, buf, len);
        if (w != (ssize_t)len)
            g_diag_dropped++;
        return;
    }
    struct pollfd p = {.fd = STDERR_FILENO, .events = POLLOUT};
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLOUT)) {
        g_diag_dropped++;
        return;
    }
    if (write(STDERR_FILENO, buf, len) != (ssize_t)len)
        g_diag_dropped++;
}

void xw_diag_vline(const char *prefix, const char *fmt, va_list ap) {
    char buf[1024];
    size_t plen = strlen(prefix);
    if (plen + 2 > sizeof(buf))
        return;
    memcpy(buf, prefix, plen);
    int n = vsnprintf(buf + plen, sizeof(buf) - plen - 1, fmt, ap);
    if (n < 0)
        return;
    size_t body = (size_t)n;
    if (body > sizeof(buf) - plen - 1)
        body = sizeof(buf) - plen - 1; /* truncated, still one line */
    buf[plen + body] = '\n';

    /* a sink that stalled reports its losses before the next line:
     * the physical runs must know the trace is incomplete */
    if (g_diag_dropped > 0) {
        long d = g_diag_dropped;
        g_diag_dropped = 0;
        char note[96];
        int nn = snprintf(note, sizeof(note),
                          "[trace] %ld diagnostic line%s dropped (stderr "
                          "stalled — output incomplete)\n",
                          d, d == 1 ? "" : "s");
        if (nn > 0)
            diag_write_raw(note, (size_t)nn < sizeof(note) - 1
                                     ? (size_t)nn
                                     : sizeof(note) - 1);
    }
    diag_write_raw(buf, plen + body + 1);
}

void xw_diag_line(const char *prefix, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xw_diag_vline(prefix, fmt, ap);
    va_end(ap);
}

void xw_log(int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;

    if (level < g_log_level)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log_fn) {
        g_log_fn(level, buf, g_log_ud);
        return;
    }
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "[xw-%s] ", level_name(level));
    xw_diag_line(prefix, "%s", buf);
}

int64_t xw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------ command split */
static char *xw_strdup_range(const char *s, size_t n) {
    char *r = malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char **xw_command_split(const char *cmdline) {
    if (!cmdline)
        return NULL;
    char **argv = NULL;
    size_t argc = 0, cap = 0;
    const char *p = cmdline;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *start = p;
        char quote = 0;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (quote) {
                if (*p == quote)
                    quote = 0;
                p++;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
                p++;
            } else {
                p++;
            }
        }
        /* strip surrounding quotes if the whole token is quoted */
        size_t n = (size_t)(p - start);
        char *tok;
        if (n >= 2 && ((start[0] == '"' && start[n - 1] == '"' && quote != '"') ||
                       (start[0] == '\'' && start[n - 1] == '\''))) {
            tok = xw_strdup_range(start + 1, n - 2);
        } else {
            tok = xw_strdup_range(start, n);
        }
        if (!tok)
            goto fail;
        if (argc + 1 >= cap) {
            cap = cap ? cap * 2 : 8;
            argv = realloc(argv, cap * sizeof(char *));
            if (!argv)
                goto fail;
        }
        /* remove in-token quote chars */
        char *dst = tok;
        for (char *src = tok; *src; src++) {
            if (*src != '"' && *src != '\'')
                *dst++ = *src;
        }
        *dst = 0;
        argv[argc++] = tok;
    }
    if (argc == 0)
        return NULL;
    argv[argc] = NULL;
    return argv;
fail:
    xw_argv_free(argv);
    return NULL;
}

void xw_argv_free(char **argv) {
    if (!argv)
        return;
    for (char **a = argv; *a; a++)
        free(*a);
    free(argv);
}

pid_t xw_spawn_command(struct xw_compositor *c, const char *cmdline) {
    if (!cmdline || !*cmdline) {
        xw_log(XW_LOG_ERROR, "spawn: empty command");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        xw_log(XW_LOG_ERROR, "spawn: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* child: restore the DEFAULT signal state before exec. The
         * compositor blocks SIGINT/SIGTERM/SIGHUP/SIGCHLD for its
         * signalfd event sources — and a blocked signal mask survives
         * both fork() and exec(). Without this, every process launched
         * here (terminals from the shortcuts, the exit-dialog helper,
         * ctl-launched apps) inherits the mask and Ctrl+C inside those
         * apps silently does nothing. Standard spawner practice. */
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        /* detach from the wayland socket fd trio; keep environment
         * (WAYLAND_DISPLAY etc.) so the launched app can connect back. */
        int nfd = open("/dev/null", O_RDWR);
        if (nfd >= 0) {
            dup2(nfd, STDIN_FILENO);
            if (nfd > 2)
                close(nfd);
        }
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    xw_log(XW_LOG_INFO, "spawn: pid %d: %s", pid, cmdline);
    xw_compositor_track_child(c, pid);
    return pid;
}

/* --------------------------------------------------------------- ini file */
static void rtrim(char *s, size_t len) {
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                   s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
}

struct xw_ini *xw_ini_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    struct xw_ini *ini = calloc(1, sizeof(*ini));
    if (!ini) {
        fclose(f);
        return NULL;
    }
    wl_list_init(&ini->sections);
    struct xw_ini_section *cur = NULL;
    char line[512];
    int lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t len = strlen(line);
        rtrim(line, len);
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) {
                xw_log(XW_LOG_WARN, "ini %s:%d: bad section", path, lineno);
                continue;
            }
            *end = 0;
            char *name = p + 1;
            while (*name == ' ')
                name++;
            char *ne = name + strlen(name);
            while (ne > name && ne[-1] == ' ')
                *--ne = 0;
            cur = calloc(1, sizeof(*cur));
            if (!cur)
                break;
            cur->name = strdup(name);
            wl_list_init(&cur->entries);
            wl_list_insert(ini->sections.prev, &cur->link);
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            xw_log(XW_LOG_WARN, "ini %s:%d: missing '='", path, lineno);
            continue;
        }
        *eq = 0;
        char *key = p, *val = eq + 1;
        char *ke = key + strlen(key);
        while (ke > key && ke[-1] == ' ')
            *--ke = 0;
        while (*val == ' ')
            val++;
        if (!cur) {
            cur = calloc(1, sizeof(*cur));
            cur->name = strdup("");
            wl_list_init(&cur->entries);
            wl_list_insert(ini->sections.prev, &cur->link);
        }
        struct xw_ini_entry *e = calloc(1, sizeof(*e));
        if (!e)
            break;
        e->key = strdup(key);
        e->value = strdup(val);
        wl_list_insert(cur->entries.prev, &e->link);
    }
    fclose(f);
    return ini;
}

const char *xw_ini_get(const struct xw_ini *ini, const char *section,
                       const char *key) {
    if (!ini)
        return NULL;
    struct xw_ini_section *s;
    wl_list_for_each(s, &ini->sections, link) {
        if (strcmp(s->name, section) != 0)
            continue;
        struct xw_ini_entry *e;
        wl_list_for_each(e, &s->entries, link) {
            if (strcmp(e->key, key) == 0)
                return e->value;
        }
    }
    return NULL;
}

void xw_ini_free(struct xw_ini *ini) {
    if (!ini)
        return;
    struct xw_ini_section *s, *s2;
    wl_list_for_each_safe(s, s2, &ini->sections, link) {
        struct xw_ini_entry *e, *e2;
        wl_list_for_each_safe(e, e2, &s->entries, link) {
            wl_list_remove(&e->link);
            free(e->key);
            free(e->value);
            free(e);
        }
        wl_list_remove(&s->link);
        free(s->name);
        free(s);
    }
    free(ini);
}
