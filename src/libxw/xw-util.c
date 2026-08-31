/* xw-util.c — logging, time, command helpers, INI parser. */
#include "xw-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    fprintf(stderr, "[xw-%s] %s\n", level_name(level), buf);
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
        /* child: detach from the wayland socket fd trio; keep environment
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
