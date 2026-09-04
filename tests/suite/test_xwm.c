/* test_xwm.c — XWayland window-manager regression suite.
 *
 * Every test here pins a defect found during the 2026-09-04
 * real-client integration round, reproduced with REAL Xwayland +
 * REAL xw-xwm processes connected to the in-process compositor, plus
 * a controllable real X11 client (tests/x11client.c, Xlib):
 *
 *   configure-mask  — ConfigureRequest events carry x/y/w/h inline
 *                     with the value-mask at offset 26; the v0 parser
 *                     read w/h from the border/mask slots, which made
 *                     xterm-sized windows come out 3x14 (the "xterm is
 *                     a few pixels big" report)
 *   identity        — WM_NAME / WM_CLASS read at MapRequest and pushed
 *                     over window-control v2 (the taskbar showed the
 *                     static "X11 window" / app 'xwayland' before)
 *   title-change    — WM_NAME PropertyNotify → new title (terminals
 *                     retitle themselves constantly)
 *   focus-routing   — compositor keyboard focus mirrored into the X
 *                     input focus (keys used to land in whichever X
 *                     window held focus at map time)
 *   focus-protocol — WM_TAKE_FOCUS delivered intact (the v0 SendEvent
 *                     set the generated-bit on the FORMAT byte: 160,
 *                     BadValue, no delivery — GTK-model apps never
 *                     learned they had focus)
 *   or-windows      — override-redirect X windows: X-owned geometry,
 *                     above managed windows, no taskbar/focus (the
 *                     OR byte in CreateNotify/ConfigureNotify was
 *                     read from padding; classification also must
 *                     not wait for a buffer commit)
 *   close-protocols — WM_DELETE_WINDOW delivered (v0 misparsed the
 *                     GetProperty reply format byte, so EVERY close
 *                     took the destroy path)
 *   close-kill      — no WM_DELETE → destroy (the honest fallback)
 *   teardown        — killing Xwayland + the helper at once used to
 *                     segfault the compositor (focus notification into
 *                     a destroyed window-control resource)
 *   xterm-real      — the REAL xterm: full-size window, real title
 *
 * Skipped (with a note) when .apps-root is not populated.
 */
#include "xwtest.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xw-internal.h"

/* ------------------------------------------------------------ scaffolding */

#define APPS "/home/z/my-project/.apps-root"
#define PROBE "/home/z/my-project/build/tests/x11client"

struct xw_proc {
    pid_t pid;
    FILE *log;
    char logpath[160];
    int in_fd; /* stdin pipe to the process (-1 = none) */
};

static bool s_apps_ok;
static int s_display_base;

static void xwm_probe_init(void) {
    s_apps_ok = access(APPS "/usr/bin/Xwayland", X_OK) == 0 &&
                access(APPS "/usr/bin/xterm", X_OK) == 0 &&
                access(PROBE, X_OK) == 0;
    /* unique display numbers per run: base 10 + pid%80 */
    s_display_base = 10 + (int)(getpid() % 80);
}

static void proc_close(struct xw_proc *p) {
    if (!p->pid)
        return;
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    p->pid = 0;
    if (p->log)
        fclose(p->log);
    p->log = NULL;
    if (p->in_fd >= 0)
        close(p->in_fd);
    p->in_fd = -1;
}

static void sock_unlink(int display) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
    unlink(path);
}

/* The in-process compositor blocks SIGHUP/SIGINT/SIGTERM/SIGCHLD for
 * its signalfd event sources (wl_event_loop_add_signal requires the
 * signal blocked) — and a blocked signal mask survives BOTH fork()
 * and exec(). Every raw fork+exec in this file must restore the
 * default signal state in the child, exactly like the product's
 * xw_spawn_command does, or the children (Xwayland, xw-xwm, the
 * x11client, xterm) are born SIGTERM-immune — a kill() then silently
 * does nothing and a waitpid() wedges the whole suite. Found live:
 * test [121] sat in wait4 forever after kill(pid, SIGTERM) because
 * xterm inherited the blocked mask. */
static void child_signal_defaults(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* per-test log tag: the trailing socket counter (xwt-<pid>-<n> → <n>).
 * The old per-display paths collided whenever two tests shared a
 * display number — later stacks truncated earlier evidence (the
 * close-delete client's DELETE line was destroyed by the xterm
 * run's stack on the same display). */
static const char *sock_tag(struct xwt_ctx *t) {
    const char *p = strrchr(t->socket_name, '-');
    return p ? p + 1 : t->socket_name;
}

/* spawn a process with stdout+stderr to a log under the runtime dir;
 * if stdin_pipe, wire a writable pipe (send NULL lines... use fd) */
static bool proc_spawn(struct xw_proc *p, struct xwt_ctx *t, int display,
                       bool with_stdin, bool is_xwm, const char *client) {
    memset(p, 0, sizeof(*p));
    p->in_fd = -1;
    char sock[32];
    snprintf(sock, sizeof(sock), "%d", display);
    snprintf(p->logpath, sizeof(p->logpath), "%s/%s-%s.log", g_runtimedir(),
             is_xwm ? "xwm" : client, sock_tag(t));
    FILE *log = fopen(p->logpath, "w");
    if (!log)
        return false;
    int in_pipe[2] = {-1, -1};
    if (with_stdin && pipe(in_pipe) < 0) {
        fclose(log);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fclose(log);
        return false;
    }
    if (pid == 0) {
        /* child */
        child_signal_defaults();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0 && !with_stdin) {
            dup2(devnull, 0);
            close(devnull);
        } else if (devnull >= 0)
            close(devnull);
        dup2(fileno(log), 1);
        dup2(fileno(log), 2);
        fclose(log);
        if (with_stdin) {
            dup2(in_pipe[0], 0);
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        /* Xwayland and the fetched X clients live in .apps-root with
         * their runtime libs (libEGL for epoxy, libXmu for xterm...) */
        setenv("LD_LIBRARY_PATH",
               APPS "/usr/lib/x86_64-linux-gnu:"
                     "/home/z/my-project/.toolchain/sysroot/usr/lib/x86_64-linux-gnu",
               1);
        char dpy[16];
        snprintf(dpy, sizeof(dpy), ":%d", display);
        if (is_xwm) {
            char wl[40];
            snprintf(wl, sizeof(wl), "%s", t->socket_name);
            setenv("WAYLAND_DISPLAY", wl, 1);
            setenv("DISPLAY", dpy, 1);
            char *const argv[] = {(char *)"/home/z/my-project/build/bin/xw-xwm",
                                  (char *)"-d", dpy, (char *)"-w", wl,
                                  (char *)"-v", NULL};
            execv(argv[0], argv);
        } else if (strcmp(client, "Xwayland") == 0) {
            setenv("WAYLAND_DISPLAY", t->socket_name, 1);
            char *const argv[] = {(char *)APPS "/usr/bin/Xwayland",
                                  (char *)dpy, (char *)"-rootless",
                                  (char *)"-noreset", NULL};
            execv(argv[0], argv);
        } else {
            setenv("DISPLAY", dpy, 1);
            unsetenv("WAYLAND_DISPLAY");
            char *const argv[] = {(char *)PROBE, (char *)dpy, NULL};
            execv(argv[0], argv);
        }
        _exit(127);
    }
    fclose(log);
    if (with_stdin) {
        close(in_pipe[0]);
        p->in_fd = in_pipe[1];
    }
    p->pid = pid;
    return true;
}

static void proc_cmd(struct xw_proc *p, const char *cmd) {
    if (p->in_fd < 0)
        return;
    ssize_t n = write(p->in_fd, cmd, strlen(cmd));
    (void)n;
}

/* wait until the X socket file for the display exists */
static bool wait_socket(int display, int iterations) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
    for (int i = 0; i < iterations; i++) {
        if (access(path, F_OK) == 0)
            return true;
        usleep(10 * 1000);
    }
    return false;
}

/* read a whole (small) log file into a buffer */
static bool log_read(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    out[0] = 0;
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = 0;
    fclose(f);
    return true;
}

/* pump the compositor while waiting for `needle` to appear in the log */
static bool wait_log(struct xwt_ctx *t, const char *path, const char *needle,
                     int iterations) {
    char buf[4096];
    for (int i = 0; i < iterations; i++) {
        if (log_read(path, buf, sizeof(buf)) && strstr(buf, needle))
            return true;
        xwt_pump(t);
        usleep(10 * 1000);
    }
    return false;
}

/* find the compositor window with a given title (managed or OR) */
static struct xw_window *win_by_title(struct xwt_ctx *t, const char *title) {
    struct xw_window *w;
    wl_list_for_each(w, &t->comp->wm->windows, link) {
        if (strcmp(w->title, title) == 0)
            return w;
    }
    wl_list_for_each(w, &t->comp->wm->or_windows, link) {
        if (strcmp(w->title, title) == 0)
            return w;
    }
    return NULL;
}

/* one full X stack: compositor (in-process) + Xwayland + xw-xwm.
 * Returns the display number, or -1 (stack torn down). */
struct xw_stack {
    struct xw_proc xwayland;
    struct xw_proc xwm;
    int display;
};

static bool stack_up(struct xwt_ctx *t, struct xw_stack *s) {
    memset(s, 0, sizeof(*s));
    s->display = s_display_base + (t->socket_name[strlen(t->socket_name) - 1] % 7);
    sock_unlink(s->display);
    if (!proc_spawn(&s->xwayland, t, s->display, false, false, "Xwayland"))
        return false;
    if (!wait_socket(s->display, 300)) {
        proc_close(&s->xwayland);
        return false;
    }
    for (int i = 0; i < 50; i++)
        xwt_pump(t); /* let Xwayland finish its Wayland handshake */
    if (!proc_spawn(&s->xwm, t, s->display, false, true, NULL)) {
        proc_close(&s->xwayland);
        return false;
    }
    char ready[160];
    snprintf(ready, sizeof(ready), "%s/xwm-%s.log", g_runtimedir(),
             sock_tag(t));
    bool ok = wait_log(t, ready, "xw-xwm ready", 400);
    if (!ok) {
        proc_close(&s->xwayland);
        proc_close(&s->xwm);
        return false;
    }
    return true;
}

static void stack_down(struct xwt_ctx *t, struct xw_stack *s) {
    (void)t;
    proc_close(&s->xwm);
    proc_close(&s->xwayland);
    sock_unlink(s->display);
}

/* spawn the x11client in map mode with extra args; appends to its log */
static bool client_spawn(struct xw_proc *p, struct xwt_ctx *t, int display,
                         const char *args) {
    (void)t;
    memset(p, 0, sizeof(*p));
    p->in_fd = -1;
    snprintf(p->logpath, sizeof(p->logpath), "%s/client-%s.log",
             g_runtimedir(), sock_tag(t));
    FILE *log = fopen(p->logpath, "w");
    if (!log)
        return false;
    int in_pipe[2];
    if (pipe(in_pipe) < 0) {
        fclose(log);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fclose(log);
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }
    if (pid == 0) {
        child_signal_defaults();
        dup2(fileno(log), 1);
        dup2(fileno(log), 2);
        fclose(log);
        dup2(in_pipe[0], 0);
        close(in_pipe[0]);
        close(in_pipe[1]);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        char dpy[16];
        snprintf(dpy, sizeof(dpy), ":%d", display);
        setenv("DISPLAY", dpy, 1);
        unsetenv("WAYLAND_DISPLAY");
        /* x11client <display> map [args...] */
        char *argv[8];
        int argc = 0;
        argv[argc++] = (char *)PROBE;
        argv[argc++] = dpy;
        argv[argc++] = (char *)"map";
        if (args && *args) {
            /* split args on spaces */
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s", args);
            for (char *tok = strtok(tmp, " "); tok && argc < 7;
                 tok = strtok(NULL, " "))
                argv[argc++] = tok;
        }
        argv[argc] = NULL;
        execv(argv[0], argv);
        _exit(127);
    }
    fclose(log);
    close(in_pipe[0]);
    p->in_fd = in_pipe[1];
    p->pid = pid;
    return true;
}

static bool client_wait(struct xwt_ctx *t, struct xw_proc *p,
                        const char *needle, int iterations) {
    char buf[4096];
    for (int i = 0; i < iterations; i++) {
        if (log_read(p->logpath, buf, sizeof(buf)) && strstr(buf, needle))
            return true;
        xwt_pump(t);
        usleep(10 * 1000);
    }
    return false;
}

/* ------------------------------------------------------------------ tests */

static void test_xwm_configure_mask(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, "200x150"));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    XWT_WAIT(t, win_by_title(t, "Probe Initial") != NULL);

    /* client-initiated resize with CWWidth|CWHeight ONLY (XResizeWindow)
     * — the v0 fixed-offset parser misread this shape */
    proc_cmd(&c, "resize 400x260\n");
    struct xw_window *w = NULL;
    for (int i = 0; i < 500 && !(w = win_by_title(t, "Probe Initial")); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(w);
    bool sized = false;
    /* the compositor models the EXTENT: Xwayland's wl_surface covers
     * the X11 window including its border (the probe maps with
     * border_width 1, exactly like real xterm). 400x260 interior +
     * 2x1 border = 402x262. */
    for (int i = 0; i < 500 && !(sized = (w->w == 402 && w->h == 262)); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(sized, "granted extent = %dx%d (want 402x262: 400x260 "
                     "interior + 2x1 border)", w->w, w->h);
    /* the granted geometry must reach the X client as ConfigureNotify */
    XWT_CHECK(client_wait(t, &c, "CONFIGURE 400x260", 400),
              "ConfigureNotify with the granted size reached the client");
    proc_cmd(&c, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_identity(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, ""));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));

    struct xw_window *w = NULL;
    bool found = false;
    for (int i = 0; i < 600 && !(found = (w = win_by_title(t, "Probe Initial"))); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(found);
    XWT_CHECK(strcmp(w->app_id, "Probe") == 0,
              "app_id = '%s' (want WM_CLASS res_class 'Probe')", w->app_id);
    XWT_CHECK(w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND,
              "the X11 window uses the xwayland surface role");
    XWT_CHECK(w->xw_has_serial, "serial correlation landed");

    proc_cmd(&c, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_title_change(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, ""));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    XWT_WAIT(t, win_by_title(t, "Probe Initial") != NULL);

    proc_cmd(&c, "title Renamed Via Escape\n");
    bool renamed = false;
    for (int i = 0; i < 600 && !(renamed = (win_by_title(t, "Renamed Via Escape") != NULL)); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(renamed, "WM_NAME PropertyNotify propagated as a title change");
    XWT_CHECK(!renamed || win_by_title(t, "Probe Initial") == NULL,
              "old title gone");

    proc_cmd(&c, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_focus_routing(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc a, b;
    XWT_ASSERT(client_spawn(&a, t, s.display, "200x150"));
    XWT_ASSERT(client_wait(t, &a, "MAPPED", 400));
    XWT_ASSERT(client_spawn(&b, t, s.display, "200x150"));
    XWT_ASSERT(client_wait(t, &b, "MAPPED", 400));
    XWT_WAIT(t, win_by_title(t, "Probe Initial") && t->comp->wm->focused);
    struct xw_window *wa = NULL, *wb = NULL;
    for (int i = 0; i < 300 && !(wa && wb); i++) {
        wa = wa ? wa : win_by_title(t, "Probe Initial");
        wb = wb ? wb : (wa && wa->id != (win_by_title(t, "Probe Initial") ? win_by_title(t, "Probe Initial")->id : 0) ? win_by_title(t, "Probe Initial") : NULL);
        if (wa && !wb) {
            /* second window: same title — find by id != wa->id */
            struct xw_window *w;
            wl_list_for_each(w, &t->comp->wm->windows, link) {
                if (w->id != wa->id && strcmp(w->title, "Probe Initial") == 0)
                    wb = w;
            }
        }
        xwt_pump(t);
        usleep(5 * 1000);
    }
    XWT_ASSERT(wa && wb);
    /* B mapped last: compositor focus = B, and the X focus must follow */
    XWT_WAIT(t, t->comp->wm->focused == wb);
    proc_cmd(&b, "queryfocus\n");
    char buf[4096];
    bool fmatch_b = false;
    for (int i = 0; i < 400; i++) {
        if (log_read(b.logpath, buf, sizeof(buf)) &&
            strstr(buf, "TAKEFOCUS"))
            break; /* wrong protocol for this client — skip */
        if (log_read(b.logpath, buf, sizeof(buf))) {
            char *f = strstr(buf, "FOCUS 0x");
            if (f) {
                unsigned long xid = strtoul(f + 8, NULL, 16);
                (void)xid;
                fmatch_b = true;
                break;
            }
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(fmatch_b, "XGetInputFocus answered (routing happened)");

    /* compositor-driven focus change: click on A through injected
     * pointer events — the X focus must move to A's X window */
    xw_wm_focus_window(t->comp->wm, wa, true);
    proc_cmd(&a, "queryfocus\n");
    bool fmatch_a = false;
    unsigned long a_xid = 0, b_xid = 0;
    for (int i = 0; i < 500; i++) {
        if (log_read(a.logpath, buf, sizeof(buf))) {
            char *f = strstr(buf, "FOCUS 0x");
            if (f) {
                a_xid = strtoul(f + 8, NULL, 16);
                fmatch_a = a_xid != 0;
                break;
            }
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(fmatch_a, "A sees an X focus reply after compositor focus");
    proc_cmd(&b, "queryfocus\n");
    for (int i = 0; i < 400; i++) {
        if (log_read(b.logpath, buf, sizeof(buf))) {
            char *f = strstr(buf, "FOCUS 0x");
            if (f) {
                b_xid = strtoul(f + 8, NULL, 16);
                break;
            }
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(a_xid && b_xid && a_xid == b_xid,
              "X focus moved to A's window (0x%lx vs B 0x%lx)", a_xid, b_xid);

    proc_cmd(&a, "exit\n");
    proc_cmd(&b, "exit\n");
    usleep(150 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&a);
    proc_close(&b);
    stack_down(t, &s);
}

static void test_xwm_override_redirect(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, ""));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    XWT_WAIT(t, win_by_title(t, "Probe Initial") != NULL);
    struct xw_window *main_w = win_by_title(t, "Probe Initial");
    XWT_ASSERT(main_w);
    XWT_WAIT(t, t->comp->wm->focused == main_w);

    /* an OR window: X owns geometry, no taskbar, no focus */
    char buf[4096];
    (void)log_read(c.logpath, buf, sizeof(buf));
    /* the client only supports map/or modes; spawn a second client in
     * or mode via a direct fork (client_spawn does map mode) */
    struct xw_proc or;
    memset(&or, 0, sizeof(or));
    or.in_fd = -1;
    snprintf(or.logpath, sizeof(or.logpath), "%s/or-%s.log", g_runtimedir(),
             sock_tag(t));
    FILE *log = fopen(or.logpath, "w");
    XWT_ASSERT(log);
    int in_pipe[2];
    XWT_ASSERT(pipe(in_pipe) == 0);
    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        child_signal_defaults();
        dup2(fileno(log), 1);
        dup2(fileno(log), 2);
        fclose(log);
        dup2(in_pipe[0], 0);
        close(in_pipe[0]);
        close(in_pipe[1]);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        char dpy[16];
        snprintf(dpy, sizeof(dpy), ":%d", s.display);
        setenv("DISPLAY", dpy, 1);
        unsetenv("WAYLAND_DISPLAY");
        char *const argv[] = {(char *)PROBE, dpy, (char *)"or",
                              (char *)"120", (char *)"80",
                              (char *)"200x100", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    fclose(log);
    close(in_pipe[0]);
    or.in_fd = in_pipe[1];
    or.pid = pid;
    XWT_ASSERT(wait_log(t, or.logpath, "MAPPED", 500));

    /* compositor side: a window flagged OR, at X-owned geometry */
    struct xw_window *orw = NULL;
    bool or_found = false;
    for (int i = 0; i < 600 &&
                    !(or_found = ((orw = win_by_title(t, "X11 window")) != NULL &&
                                  orw->xw_override_redirect));
         i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(or_found, "override-redirect window classified");
    if (or_found) {
        XWT_CHECK(orw->x == 120 && orw->y == 80,
                  "OR window at X geometry %d+%d (want 120+80)", orw->x,
                  orw->y);
        bool in_managed = false;
        struct xw_window *w;
        wl_list_for_each(w, &t->comp->wm->windows, link) {
            if (w == orw)
                in_managed = true;
        }
        XWT_CHECK(!in_managed, "OR window not in the managed set");
        XWT_CHECK(wl_list_empty(&orw->toplevel_handles),
                  "OR window has no taskbar (foreign-toplevel) handle");
        /* focus must NOT move to the OR window */
        XWT_CHECK(t->comp->wm->focused == main_w,
                  "keyboard focus stayed on the managed window");
        /* interactive move/resize refused */
        XWT_CHECK(!xw_wm_interactive_begin_move(t->comp->wm, orw, 0, 0),
                  "interactive move refused for OR window");
    }
    proc_close(&or);
    proc_cmd(&c, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_focus_protocol(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    /* a WM_TAKE_FOCUS client (the GTK/ICCCM active input model): the
     * protocol message must actually ARRIVE. The v0 SendEvent set the
     * "generated" bit on the FORMAT byte (32|0x80 = 160) — an invalid
     * ClientMessage format, BadValue, and the message never reached
     * the client: take-focus apps never learned they had focus. Real
     * xterm is passive (WM_DELETE only, verified with xprop); GTK is
     * the active model, the probe's takefocus flag reproduces it. */
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, "200x150 takefocus"));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    struct xw_window *w = NULL;
    bool found = false;
    for (int i = 0; i < 600 && !(found = (w = win_by_title(t, "Probe Initial")));
         i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(found);
    /* the compositor focuses the newly mapped window; the helper must
     * deliver WM_TAKE_FOCUS (with input hint true it ALSO runs
     * SetInputFocus — the active model still wants the message) */
    XWT_CHECK(client_wait(t, &c, "TAKEFOCUS", 600),
              "WM_TAKE_FOCUS delivered to a take-focus client");
    proc_cmd(&c, "queryfocus\n");
    char buf[4096];
    bool fok = false;
    for (int i = 0; i < 400; i++) {
        if (log_read(c.logpath, buf, sizeof(buf))) {
            char *f = strstr(buf, "FOCUS 0x");
            if (f && strtoul(f + 8, NULL, 16) != 0) {
                fok = true;
                break;
            }
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(fok, "X input focus set for the take-focus window");
    proc_cmd(&c, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_close_delete(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, ""));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    struct xw_window *w = NULL;
    bool found = false;
    for (int i = 0; i < 600 && !(found = (w = win_by_title(t, "Probe Initial"))); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(found && w->xw_has_serial);

    /* the taskbar close path: WM_DELETE_WINDOW must be delivered (v0
     * misparsed the GetProperty reply and always destroyed) */
    xw_xwayland_window_close(w);
    XWT_CHECK(wait_log(t, c.logpath, "DELETE", 600),
              "WM_DELETE_WINDOW delivered to a supporting client");
    XWT_CHECK(wait_log(t, c.logpath, "EXIT", 600), "client exited cleanly");
    int status = 0;
    for (int i = 0; i < 200; i++) {
        pid_t r = waitpid(c.pid, &status, WNOHANG);
        if (r == c.pid) {
            c.pid = 0;
            break;
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_close_kill(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, "nodelete"));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    struct xw_window *w = NULL;
    bool found = false;
    for (int i = 0; i < 600 && !(found = (w = win_by_title(t, "Probe Initial"))); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(found && w->xw_has_serial);

    xw_xwayland_window_close(w);
    /* no WM_DELETE support: the window is destroyed; the client sees
     * DestroyNotify and exits */
    bool exited = false;
    for (int i = 0; i < 600; i++) {
        pid_t r = waitpid(c.pid, NULL, WNOHANG);
        if (r == c.pid) {
            c.pid = 0;
            exited = true;
            break;
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(exited, "client without WM_DELETE support exits on destroy");
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_teardown(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, ""));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    XWT_WAIT(t, win_by_title(t, "Probe Initial") != NULL);
    struct xw_window *w = win_by_title(t, "Probe Initial");
    XWT_WAIT(t, w && t->comp->wm->focused == w);

    /* kill Xwayland AND the helper at once (the logout path): the
     * compositor must survive the focus notifications fired from the
     * dying surfaces (v0 posted them into the destroyed manager) */
    kill(s.xwayland.pid, SIGKILL);
    kill(s.xwm.pid, SIGKILL);
    s.xwayland.pid = 0;
    s.xwm.pid = 0;
    for (int i = 0; i < 300; i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(!t->client_dead, "test client connection healthy");
    XWT_CHECK(wl_list_empty(&t->comp->wc_managers),
              "window-control manager list drained");
    /* the compositor must still work: map a native window after the
     * X stack died */
    struct xwc_win *nw = xwt_window_solid(t, 0x204060ff, 100, 60, "after");
    XWT_WAIT(t, win_by_title(t, "after") && win_by_title(t, "after")->mapped);
    XWT_CHECK(win_by_title(t, "after") != NULL,
              "native window maps after XWayland death");
    (void)nw;
    proc_close(&c);
    stack_down(t, &s);
}

static void test_xwm_xterm_real(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    /* the REAL xterm: font-metric-driven resize over a size-only
     * ConfigureRequest (the original 3x14 symptom) */
    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        child_signal_defaults();
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        setenv("LD_LIBRARY_PATH",
               APPS "/usr/lib/x86_64-linux-gnu", 1);
        char dpy[16];
        snprintf(dpy, sizeof(dpy), ":%d", s.display);
        setenv("DISPLAY", dpy, 1);
        unsetenv("WAYLAND_DISPLAY");
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        /* argv[0] MUST be absolute: xterm validates its own program
         * path ("No absolute path found for shell: xterm" — it treats
         * argv[0] as the thing it re-execs and refuses relative
         * names), then never maps a window. Found with the real
         * binary; absolute argv[0] is what every real launcher
         * (terminals, .desktop Exec) passes anyway. */
        execl(APPS "/usr/bin/xterm", APPS "/usr/bin/xterm", NULL);
        _exit(127);
    }
    struct xw_window *w = NULL;
    bool found = false;
    for (int i = 0; i < 800 && !(found = (w = win_by_title(t, "xterm"))); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(found, "real xterm mapped");
    if (found) {
        bool sized = false;
        for (int i = 0; i < 600 && !(sized = (w->w > 100 && w->h > 100)); i++) {
            xwt_pump(t);
            usleep(10 * 1000);
        }
        XWT_CHECK(sized, "xterm real size %dx%d (want > 100x100)", w->w, w->h);
        XWT_CHECK(strcmp(w->app_id, "XTerm") == 0,
                  "xterm app_id = '%s' (want XTerm)", w->app_id);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    for (int i = 0; i < 150; i++)
        xwt_pump(t);
    stack_down(t, &s);
}

static const struct xwt_test tests[] = {
    {"xwm-configure-mask", test_xwm_configure_mask},
    {"xwm-identity", test_xwm_identity},
    {"xwm-title-change", test_xwm_title_change},
    {"xwm-focus-routing", test_xwm_focus_routing},
    {"xwm-focus-protocol", test_xwm_focus_protocol},
    {"xwm-override-redirect", test_xwm_override_redirect},
    {"xwm-close-delete", test_xwm_close_delete},
    {"xwm-close-kill", test_xwm_close_kill},
    {"xwm-teardown", test_xwm_teardown},
    {"xwm-xterm-real", test_xwm_xterm_real},
};

__attribute__((constructor)) static void register_tests(void) {
    xwm_probe_init();
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
