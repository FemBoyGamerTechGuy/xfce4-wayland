/* panel-launch.c — direct .desktop application launching for the panel.
 *
 * The freedesktop-correct launch path: the Exec line was already parsed
 * into an argv array per the desktop-entry spec (panel-apps.c); this
 * module executes that argv DIRECTLY with posix_spawn —
 *
 *   - no /bin/sh -c (desktop entries are input data, not shell code;
 *     quoting/escapes are handled by the spec parser, not a shell)
 *   - no session-manager round trip (the panel no longer depends on
 *     the session's ctl socket to start an app; the session stays a
 *     supervisor, not a launcher broker)
 *   - POSIX_SPAWN_SETSIGDEF|SETSIGMASK resets the signal state so the
 *     child's own Ctrl+C/SIGTERM semantics work (a blocked-inherited
 *     mask used to break exactly that)
 *   - stdio on /dev/null; the environment is inherited (WAYLAND_DISPLAY,
 *     XDG_*) so the app connects back to the compositor by itself
 *
 * The spawned process is a normal child; the panel's existing SIGCHLD
 * reaper (waitpid loop, xw-panel.c) collects it, so a crashing or
 * immediately-exiting application can never hurt the panel. All
 * failures return false with a reason; the caller reports them visibly.
 */
#include "panel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* the /dev/null fd for child stdio, opened once per panel process */
static int g_nullfd = -1;

static int nullfd(void) {
    if (g_nullfd < 0)
        g_nullfd = open("/dev/null", O_RDWR);
    return g_nullfd;
}

void panel_launch_shutdown(void) {
    if (g_nullfd >= 0) {
        close(g_nullfd);
        g_nullfd = -1;
    }
}

/* Escape a string into a safe diagnostic form: control characters and
 * non-printable bytes become \xNN so a log line can never carry hidden
 * payload (and never leaks secrets verbatim from odd desktop files). */
static void diag_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in;
         *p && o + 5 < n; p++) {
        if (*p >= 0x20 && *p < 0x7f) {
            out[o++] = (char)*p;
        } else {
            o += (size_t)snprintf(out + o, n - o, "\\x%02x", *p);
        }
    }
    out[o] = 0;
}

bool panel_spawn_argv(char args[][XWAPP_ARG_MAX], int n, pid_t *pid_out,
                      char *err, size_t err_n) {
    if (err && err_n)
        err[0] = 0;
    if (n < 1 || !args[0][0]) {
        snprintf(err, err_n, "empty command");
        return false;
    }

    /* executable sanity: an absolute/relative path must exist and be
     * executable (posix_spawnp would report this only via the child's
     * exit status, which is silent to the panel) */
    if (strchr(args[0], '/')) {
        struct stat st;
        if (stat(args[0], &st) != 0) {
            char safe[XWAPP_ARG_MAX + 8];
            diag_escape(args[0], safe, sizeof(safe));
            snprintf(err, err_n, "executable not found: %s", safe);
            return false;
        }
        if (access(args[0], X_OK) != 0) {
            char safe[XWAPP_ARG_MAX + 8];
            diag_escape(args[0], safe, sizeof(safe));
            snprintf(err, err_n, "not executable: %s", safe);
            return false;
        }
    }

    /* argv vector: pointers into the caller's arrays (valid for the
     * duration of the call — posix_spawn copies what it needs) */
    char *argv[XWAPP_MAX_ARGS + 1];
    if (n > XWAPP_MAX_ARGS)
        n = XWAPP_MAX_ARGS;
    for (int i = 0; i < n; i++)
        argv[i] = args[i];
    argv[n] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int nf = nullfd();
    if (nf >= 0) {
        posix_spawn_file_actions_adddup2(&fa, nf, STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, nf, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, nf, STDERR_FILENO);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* reset the signal state: the panel blocks/reaps SIGCHLD and traps
     * SIGINT/TERM/HUP — none of that may leak into applications */
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    sigset_t dfl;
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGHUP);
    sigaddset(&dfl, SIGCHLD);
    sigaddset(&dfl, SIGQUIT);
    sigaddset(&dfl, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF |
                                        POSIX_SPAWN_SETSIGMASK);

    pid_t pid = -1;
    int rc = 0;
    if (strchr(args[0], '/'))
        rc = posix_spawn(&pid, args[0], &fa, &attr, argv, environ);
    else
        rc = posix_spawnp(&pid, args[0], &fa, &attr, argv, environ);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        char safe[XWAPP_ARG_MAX + 8];
        diag_escape(args[0], safe, sizeof(safe));
        snprintf(err, err_n, "spawn %s: %s", safe, strerror(rc));
        return false;
    }
    if (pid_out)
        *pid_out = pid;
    return true;
}

/* the menu-facing entry: full diagnostics + the visible failure state.
 * Phase-3 ordering is deliberate: the launch data is COPIED first (the
 * struct xwapp lives in the panel's database and outlives the menu, but
 * the copy makes that explicit and immune to future lifetime changes),
 * the process is spawned, and only then is the menu closed — the click
 * callback's data is never freed under it. */
bool panel_launch_app(struct panel *p, const struct xwapp *app) {
    char name[XWAPP_NAME_MAX];
    char desktop[XWAPP_PATH_MAX];
    char exec_diag[XWAPP_EXEC_MAX * 2];
    char args[XWAPP_MAX_ARGS][XWAPP_ARG_MAX];
    char err[192];

    snprintf(name, sizeof(name), "%.120s", app->name);
    snprintf(desktop, sizeof(desktop), "%.500s", app->path);
    diag_escape(app->exec, exec_diag, sizeof(exec_diag));

    fprintf(stderr, "xw-panel: launch requested: %s\n", name);
    fprintf(stderr, "xw-panel: desktop file: %s\n", desktop);
    fprintf(stderr, "xw-panel: exec: %s\n", exec_diag);

    int n = xwapp_launch_argv(app, args, XWAPP_MAX_ARGS, err, sizeof(err));
    if (n < 1) {
        fprintf(stderr, "xw-panel: cannot launch '%s': %s\n", name,
                err[0] ? err : "unusable entry");
        snprintf(p->launch_err, sizeof(p->launch_err), "cannot launch %.60s",
                 name);
        p->launch_err_ms = panel_mono_ms();
        p->redraw = true;
        return false;
    }

    pid_t pid = -1;
    fprintf(stderr, "xw-panel: spawning application\n");
    if (!panel_spawn_argv(args, n, &pid, err, sizeof(err))) {
        fprintf(stderr, "xw-panel: launch failed: %s\n",
                err[0] ? err : "spawn error");
        snprintf(p->launch_err, sizeof(p->launch_err), "%.90s",
                 err[0] ? err : "launch failed");
        p->launch_err_ms = panel_mono_ms();
        p->redraw = true;
        return false;
    }
    fprintf(stderr, "xw-panel: child pid=%d\n", (int)pid);
    fprintf(stderr, "xw-panel: launch complete\n");

    /* transient status line on the bar (cleared by draw() after ~2.5s) */
    snprintf(p->launch_err, sizeof(p->launch_err), "launched %.60s", name);
    p->launch_err_ms = panel_mono_ms();
    p->redraw = true;
    return true;
}
