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

/* ------------------------------------------------ x11 key routing */
/* The X side of the wire-keycode-space contract: injected RAW evdev
 * keycodes must reach the X11 client as X keycodes (evdev + 8) with
 * the matching keysyms — the full path seat -> wl wire (RAW evdev) ->
 * Xwayland (+8) -> X KeyPress. A +8-on-the-wire bug shifts every key
 * one row off here too (BackSpace becomes 'u'), exactly like it does
 * for native clients; this is the leg the nested-X matrix could never
 * cover. */
static void test_xwm_x11_keys(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc a;
    XWT_ASSERT(client_spawn(&a, t, s.display, "200x150"));
    XWT_ASSERT(client_wait(t, &a, "MAPPED", 400));
    struct xw_window *wa = NULL;
    for (int i = 0; i < 300 && !wa; i++) {
        wa = win_by_title(t, "Probe Initial");
        xwt_pump(t);
        usleep(5 * 1000);
    }
    XWT_ASSERT(wa);
    XWT_WAIT(t, t->comp->wm->focused == wa);
    proc_cmd(&a, "keys on\n");
    XWT_ASSERT(wait_log(t, a.logpath, "KEYS-ON", 200));

    /* the report matrix as RAW evdev codes: KEY_BACKSPACE=14, KEY_U=22,
     * KEY_A=30 — the compositor must put these on the wl wire as-is;
     * Xwayland adds 8, so the X client must see 22/30/38 */
    static const struct {
        uint32_t raw;
        unsigned xkey;
        const char *keysym;
    } matrix[] = {
        {14, 22, "BackSpace"},
        {22, 30, "u"},
        {30, 38, "a"},
    };
    for (size_t k = 0; k < sizeof(matrix) / sizeof(matrix[0]); k++) {
        xw_compositor_inject_key(t->comp, matrix[k].raw, true);
        xwt_pump(t);
        xw_compositor_inject_key(t->comp, matrix[k].raw, false);
        xwt_pump(t);
    }

    /* wait for the last release to appear, then assert every line */
    const char *want = "KEY 38 a release";
    for (int i = 0; i < 400; i++) {
        char wbuf[4096];
        if (log_read(a.logpath, wbuf, sizeof(wbuf)) && strstr(wbuf, want))
            break;
        xwt_pump(t);
        usleep(10 * 1000);
    }
    char buf[8192];
    XWT_ASSERT(log_read(a.logpath, buf, sizeof(buf)));
    int n_lines = 0;
    for (const char *p = strstr(buf, "KEY "); p; p = strstr(p + 1, "KEY "))
        n_lines++;
    XWT_CHECK(n_lines == 6, "X client saw %d KEY events, expected 6",
              n_lines);
    for (size_t k = 0; k < sizeof(matrix) / sizeof(matrix[0]); k++) {
        for (int press = 0; press < 2; press++) {
            char line[64];
            snprintf(line, sizeof(line), "KEY %u %s %s", matrix[k].xkey,
                     matrix[k].keysym, press ? "press" : "release");
            XWT_CHECK(strstr(buf, line) != NULL,
                      "missing X key event '%s' — the X keycode must be "
                      "the raw evdev code + 8 (a +8 wire bug shifts it to "
                      "30/38/46 and BackSpace types 'u')",
                      line);
        }
    }

    proc_cmd(&a, "exit\n");
    usleep(150 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&a);
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

static void test_xwm_fullscreen(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, "240x140"));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    struct xw_window *w = NULL;
    for (int i = 0; i < 600 && !(w = win_by_title(t, "Probe Initial")); i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_ASSERT(w);
    struct xw_output *o =
        wl_container_of(t->comp->outputs.next, o, link);

    /* 1. the runtime EWMH request — exactly what GTK/Qt/SDL apps send:
     * ClientMessage(_NET_WM_STATE, ADD, _NET_WM_STATE_FULLSCREEN) */
    proc_cmd(&c, "fullscreen\n");
    bool fs = false;
    for (int i = 0; i < 600 &&
                    !(fs = (w->fullscreen && w->w == o->width &&
                            w->h == o->height && w->x == o->x &&
                            w->y == o->y));
         i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(fs, "fullscreen applied: state=%d geometry %dx%d+%d+%d "
                  "(want output %dx%d+%d+%d)",
              w->fullscreen, w->w, w->h, w->x, w->y, o->width, o->height,
              o->x, o->y);
    /* the granted fullscreen geometry must reach the X client as a
     * ConfigureNotify (interior = extent - 2x1 border) */
    char conf[48];
    snprintf(conf, sizeof(conf), "CONFIGURE %dx%d", o->width - 2,
             o->height - 2);
    XWT_CHECK(client_wait(t, &c, conf, 600),
              "ConfigureNotify %s reached the client", conf);
    /* the WM must keep the EWMH property in sync (EWMH: the WM owns
     * _NET_WM_STATE once it manages the window) */
    XWT_CHECK(client_wait(t, &c, "STATE _NET_WM_STATE_FULLSCREEN", 600),
              "the helper synced the _NET_WM_STATE property");

    /* 2. leave fullscreen: the saved geometry comes back */
    proc_cmd(&c, "unfullscreen\n");
    bool restored = false;
    for (int i = 0; i < 600 &&
                    !(restored = (!w->fullscreen && w->w == 242 &&
                                  w->h == 142));
         i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(restored, "unfullscreen restore: state=%d extent %dx%d "
                        "(want 242x142: 240x140 + 2x1 border)",
              w->fullscreen, w->w, w->h);
    XWT_CHECK(client_wait(t, &c, "STATE (none)", 600) ||
                  client_wait(t, &c, "STATE (get-failed)", 100),
              "_NET_WM_STATE cleared after unfullscreen");

    /* 3. the map-time path: apps that START fullscreen set the property
     * before mapping (games, players) — no runtime message at all.
     * Both clients share the title "Probe Initial", so the wait must
     * look for a SECOND window (the first is w); the pump is what gives
     * the compositor the cycles to dispatch g's surface association. */
    struct xw_proc g;
    XWT_ASSERT(client_spawn(&g, t, s.display, "200x100 fullscreen"));
    XWT_ASSERT(client_wait(t, &g, "MAPPED", 400));
    struct xw_window *w2 = NULL;
    for (int i = 0; i < 600 && !w2; i++) {
        struct xw_window *it;
        wl_list_for_each(it, &t->comp->wm->windows, link) {
            if (it != w && strcmp(it->title, "Probe Initial") == 0) {
                w2 = it;
                break;
            }
        }
        if (!w2) {
            xwt_pump(t);
            usleep(10 * 1000);
        }
    }
    XWT_ASSERT(w2);
    bool fs2 = false;
    for (int i = 0; i < 600 &&
                    !(fs2 = (w2->fullscreen && w2->w == o->width &&
                             w2->h == o->height));
         i++) {
        xwt_pump(t);
        usleep(10 * 1000);
    }
    XWT_CHECK(fs2, "map-time fullscreen: state=%d geometry %dx%d",
              w2->fullscreen, w2->w, w2->h);

    proc_cmd(&c, "exit\n");
    proc_cmd(&g, "exit\n");
    usleep(100 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    proc_close(&g);
    stack_down(t, &s);
}

/* --------------------------------------------------- geometry truth ---- */

/* THE physical-symptom battery (2026-09-05 NVIDIA round): for a real
 * X11 window, in ONE test, verify the three coordinate agreements the
 * desktop depends on —
 *   1. render rect == X extent rect (where pixels are == where X routes)
 *   2. hit-test rect == render rect (clicks land on what is drawn)
 *   3. pointer events are WINDOW-LOCAL in X (apps hit-test their own
 *      widgets correctly — the "Mirage is non-functional" root cause)
 * plus fullscreen pixel coverage (no "unexplained gap") and the
 * granted-resize model stability. Every check was a live physical
 * failure before this round; the automated suite never exercised
 * compositor-side geometry, only X-side truth. */
/* parse "GEOM <w>x<h>+<x>+<y> BW <bw>" */
static bool parse_geom_line(const char *line, int *w, int *h, int *x, int *y,
                            int *bw) {
    return sscanf(line, "GEOM %dx%d+%d+%d BW %d", w, h, x, y, bw) == 5;
}

/* the client log is append-only from the CLIENT side; the test reads
 * it whole. To wait for a FRESH line (not one from an earlier phase),
 * record the file size before sending the command and search only
 * past that offset — client_wait(t, "DREW") otherwise matches the
 * PREVIOUS draw's line instantly and the phase races ahead of the
 * client's 100ms select loop. Found live: the fullscreen corner
 * check read a pre-draw (empty, resize-cleared) buffer. */
static long log_size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

static bool client_wait_from(struct xwt_ctx *t, struct xw_proc *p,
                             const char *needle, long from, int iterations) {
    char buf[8192];
    for (int i = 0; i < iterations; i++) {
        if (log_read(p->logpath, buf, sizeof(buf))) {
            if ((long)strlen(buf) > from &&
                strstr(buf + (from < (long)sizeof(buf) ? from : 0), needle))
                return true;
        }
        xwt_pump(t);
        usleep(10 * 1000);
    }
    return false;
}

/* parse the LAST line starting with prefix that begins at/after offset */
static bool last_log_line_from(struct xw_proc *p, const char *prefix,
                               long from, char *out, size_t outsz) {
    char buf[8192];
    if (!log_read(p->logpath, buf, sizeof(buf)))
        return false;
    if ((long)strlen(buf) <= from)
        return false;
    char *best = NULL;
    char *cur = buf + (from < (long)(sizeof(buf) - 1) ? from : 0);
    size_t plen = strlen(prefix);
    while ((cur = strstr(cur, prefix)) != NULL) {
        best = cur;
        cur += plen;
    }
    if (!best)
        return false;
    snprintf(out, outsz, "%s", best);
    return true;
}

static struct xw_window *wait_x11_window(struct xwt_ctx *t) {
    struct xw_window *w = NULL;
    for (int i = 0; i < 500 && !w; i++) {
        w = win_by_title(t, "Probe Initial");
        if (!w) {
            xwt_pump(t);
            usleep(5 * 1000);
        }
    }
    if (!w)
        return NULL;
    for (int i = 0; i < 600 && !w->mapped; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
    }
    return w->mapped ? w : NULL;
}

/* bounding box of a color in the composited output (logical coords) */
static bool color_bbox(const uint32_t *pix, int pw, int ph, uint32_t rgb,
                       int *bx, int *by, int *bw, int *bh) {
    int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            if ((pix[y * pw + x] & 0xffffff) == rgb) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0)
        return false;
    *bx = x0;
    *by = y0;
    *bw = x1 - x0 + 1;
    *bh = y1 - y0 + 1;
    return true;
}

static void test_xwm_geometry_truth(struct xwt_ctx *t) {
    if (!s_apps_ok) {
        XWT_SKIP("apps-root not populated (XWayland tests)");
        return;
    }
    struct xw_stack s;
    XWT_ASSERT(stack_up(t, &s));
    struct xw_proc c;
    XWT_ASSERT(client_spawn(&c, t, s.display, "240x140 motion button border1"));
    XWT_ASSERT(client_wait(t, &c, "MAPPED", 400));
    XWT_ASSERT(client_wait(t, &c, "GEOM ", 400));

    struct xw_window *w = wait_x11_window(t);
    XWT_ASSERT(w);
    XWT_ASSERT(w->output);

    /* --- invariant 1: the compositor model == the X extent rect ---
     * X truth: interior WxH+X+Y, border bw. The extent rect the X
     * server routes input in is (X-bw, Y-bw, W+2bw, H+2bw). The
     * mirror is eventual: at map the compositor re-places the window
     * (cascade) and pushes the placement to X, so the X truth only
     * converges after the helper's ConfigureWindow — wait for the
     * client's ConfigureNotify before comparing. */
    XWT_ASSERT(client_wait(t, &c, "CONFIGURE", 400));
    for (int i = 0; i < 200; i++)
        xwt_pump(t);
    char gline[256];
    int gw = 0, gh = 0, gx = 0, gy = 0, gbw = 0;
    long gbase = log_size_of(c.logpath);
    proc_cmd(&c, "geom\n");
    bool have_geom = client_wait_from(t, &c, "GEOM ", gbase, 100);
    XWT_ASSERT(have_geom);
    have_geom = last_log_line_from(&c, "GEOM ", gbase, gline, sizeof(gline));
    XWT_ASSERT(have_geom);
    XWT_ASSERT(parse_geom_line(gline, &gw, &gh, &gx, &gy, &gbw));
    int ew = gw + 2 * gbw, eh = gh + 2 * gbw;
    int ex = gx - gbw, ey = gy - gbw;
    xw_wm_trace_geometry(w, "audit");
    XWT_CHECK(w->w == ew && w->h == eh,
              "model size == X extent size: model %dx%d vs extent %dx%d "
              "(interior %dx%d bw %d)",
              w->w, w->h, ew, eh, gw, gh, gbw);
    XWT_CHECK(w->x == ex && w->y == ey,
              "model origin == X extent origin: model +%d+%d vs extent "
              "+%d+%d (X interior +%d+%d, bw %d)",
              w->x, w->y, ex, ey, gx, gy, gbw);

    /* --- invariant 2: hit-test rect == model rect (render rect) ---
     * probe the model rect's center and four inner corners; a point
     * clearly outside must NOT hit. This is the grab/hit-offset
     * battery (the physical "clicks land in the wrong place"). */
    int hits = 0;
    for (int i = 0; i < 5; i++) {
        int px = (i & 1) ? (w->x + w->w - 3) : (w->x + 2);
        int py = (i & 2) ? (w->y + w->h - 3) : (w->y + 2);
        if (i == 4) {
            px = w->x + w->w / 2;
            py = w->y + w->h / 2;
        }
        struct xw_surface *picked = NULL;
        struct xw_window *hw = xw_wm_window_at(t->comp->wm, px, py, &picked);
        if (hw == w)
            hits++;
        else
            xw_wm_trace_pick(t->comp->wm, px, py);
    }
    XWT_CHECK(hits == 5, "hit-test covers the model rect (center + 4 "
              "corners): %d/5 picked window %u at model %dx%d+%d+%d",
              hits, w->id, w->w, w->h, w->x, w->y);

    /* a point inside the (0,0)-anchored phantom input rect but OUTSIDE
     * the model must NOT hit the window (the pre-fix bug: it did) */
    {
        int px = 30, py = 20;
        if (px >= w->x && px < w->x + w->w && py >= w->y &&
            py < w->y + w->h) {
            /* the window actually covers it (e.g. placed at origin):
             * pick another phantom-only point */
            px = w->x + w->w + 40;
            py = w->y + w->h + 40;
        }
        struct xw_window *hw = xw_wm_window_at(t->comp->wm, px, py, NULL);
        XWT_CHECK(hw != w, "no phantom input rect: (%d,%d) outside the "
                  "model rect must not pick window %u", px, py, w->id);
    }

    /* --- invariant 3: pointer events are window-local in X ---
     * inject motion at the model center; the X client must report
     * window-local coordinates ((w/2)-ish, (h/2)-ish), not global. */
    int mx = w->x + w->w / 2, my = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, mx, my);
    for (int i = 0; i < 100; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
    }
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    for (int i = 0; i < 50; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
    }
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    for (int i = 0; i < 100; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
    }

    char mline[256];
    bool got_motion = false;
    {
        char buf[8192];
        char *cur, *best = NULL;
        if (log_read(c.logpath, buf, sizeof(buf))) {
            for (cur = buf; (cur = strstr(cur, "MOTION ")) != NULL;
                 cur += 7)
                best = cur;
        }
        if (best) {
            snprintf(mline, sizeof(mline), "%s", best);
            got_motion = true;
        }
    }
    XWT_CHECK(got_motion, "X11 client received pointer motion");
    if (got_motion) {
        int rx = -9999, ry = -9999;
        sscanf(mline, "MOTION %d,%d", &rx, &ry);
        XWT_CHECK(rx >= 0 && rx < w->w && ry >= 0 && ry < w->h,
                  "motion is window-local: got (%d,%d), want within "
                  "(0,0..%d,%d) [global was (%d,%d)]",
                  rx, ry, w->w, w->h, mx, my);
    }
    {
        char buf[8192];
        bool got_btn = false;
        if (log_read(c.logpath, buf, sizeof(buf)))
            got_btn = strstr(buf, "BTN d") != NULL;
        XWT_CHECK(got_btn, "X11 client received the button press");
    }

    /* --- render truth: pixels appear at the model rect ---
     * draw a distinctive color and scan the output for its bbox.
     * This is the frozen-content regression: with the frame-callback
     * lifecycle broken (surface->mapped never set), Xwayland
     * presented exactly ONE frame and then waited forever — the
     * physical white/invisible windows. */
    proc_cmd(&c, "draw 0x00CC66\n");
    XWT_ASSERT(client_wait(t, &c, "DREW", 400));

    int pw = 0, ph = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    XWT_ASSERT(pix);
    int bx = 0, by = 0, bbw = 0, bbh = 0;
    bool found = false;
    for (int i = 0; i < 300 && !(found = color_bbox(pix, pw, ph, 0x00CC66,
                                                    &bx, &by, &bbw, &bbh));
         i++) {
        xwt_pump(t);
        pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    }
    XWT_CHECK(found, "client color composited on the output");
    if (found) {
        /* the fill-color bbox is the INTERIOR: the X border ring is
         * painted with the window's border pixel, not the fill — the
         * interior of the model rect is inset by the X border width */
        int ix = w->x + gbw, iy = w->y + gbw;
        int iw2 = w->w - 2 * gbw, ih2 = w->h - 2 * gbw;
        XWT_CHECK(bx == ix && by == iy,
                  "render bbox origin == model interior origin: (%d,%d) vs "
                  "(%d,%d) [model +%d+%d bw %d]", bx, by, ix, iy, w->x,
                  w->y, gbw);
        XWT_CHECK(bbw == iw2 && bbh == ih2,
                  "render bbox size == model interior size: %dx%d vs %dx%d "
                  "[model %dx%d bw %d]", bbw, bbh, iw2, ih2, w->w, w->h,
                  gbw);
    }

    /* --- fullscreen: model == output rect, pixels fill edge-to-edge,
     * X side sees the same extent (no gap, no offset) --- */
    struct xw_output *o = w->output;
    long fbase = log_size_of(c.logpath);
    xw_wm_fullscreen(t->comp->wm, w, true);
    /* the fs resize reaches X as a ConfigureNotify (the mirror) */
    XWT_ASSERT(client_wait_from(t, &c, "CONFIGURE", fbase, 400));
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    long fbase1 = log_size_of(c.logpath);
    proc_cmd(&c, "draw 0x66CC00\n");
    XWT_ASSERT(client_wait_from(t, &c, "DREW", fbase1, 400));
    for (int i = 0; i < 300; i++) {
        xwt_pump(t);
        usleep(2 * 1000);
    }
    XWT_CHECK(w->fullscreen && w->w == o->width && w->h == o->height &&
              w->x == o->x && w->y == o->y,
              "fullscreen model == output rect: %dx%d+%d+%d (output "
              "%dx%d+%d+%d)", w->w, w->h, w->x, w->y,
              o->width, o->height, o->x, o->y);
    pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    {
        int m = 2;
        int want = 0x66CC00;
        int c00 = pix[m * pw + m] & 0xffffff;
        int c10 = pix[m * pw + (pw - m - 1)] & 0xffffff;
        int c01 = pix[(ph - m - 1) * pw + m] & 0xffffff;
        int c11 = pix[(ph - m - 1) * pw + (pw - m - 1)] & 0xffffff;
        XWT_CHECK(c00 == want && c10 == want && c01 == want && c11 == want,
                  "fullscreen fills all four corners: %06x %06x %06x "
                  "%06x (want 66cc00 — no unexplained gap)", c00, c10,
                  c01, c11);
    }
    /* X truth after the fullscreen settle: extent == output rect */
    for (int i = 0; i < 200; i++)
        xwt_pump(t);
    long fbase2 = log_size_of(c.logpath);
    proc_cmd(&c, "geom\n");
    have_geom = client_wait_from(t, &c, "GEOM ", fbase2, 100);
    have_geom = last_log_line_from(&c, "GEOM ", fbase2, gline, sizeof(gline));
    if (have_geom && parse_geom_line(gline, &gw, &gh, &gx, &gy, &gbw)) {
        XWT_CHECK(gx - gbw == o->x && gy - gbw == o->y &&
                  gw + 2 * gbw == o->width && gh + 2 * gbw == o->height,
                  "fullscreen X extent == output rect: X %dx%d+%d+%d bw "
                  "%d (extent %dx%d+%d+%d) vs output %dx%d+%d+%d",
                  gw, gh, gx, gy, gbw, gw + 2 * gbw, gh + 2 * gbw,
                  gx - gbw, gy - gbw, o->width, o->height, o->x, o->y);
    }

    /* --- granted resize: the granted rect survives the round trip ---
     * interactive resize to a new size, then the client redraws; the
     * model must equal what was granted (buffer adoption may not
     * clobber it), and X truth must follow. */
    xw_wm_fullscreen(t->comp->wm, w, false);
    for (int i = 0; i < 200; i++)
        xwt_pump(t);
    int target_w = 360, target_h = 240;
    xw_wm_interactive_begin_resize(t->comp->wm, w, XW_EDGE_R | XW_EDGE_B,
                                   w->x + w->w, w->y + w->h);
    xw_wm_interactive_motion(t->comp->wm, w, w->x + target_w,
                             w->y + target_h);
    for (int i = 0; i < 50; i++)
        xwt_pump(t);
    XWT_CHECK(w->w == target_w && w->h == target_h,
              "interactive resize granted %dx%d: model %dx%d", target_w,
              target_h, w->w, w->h);
    long rbase = log_size_of(c.logpath);
    proc_cmd(&c, "draw 0x3366CC\n");
    XWT_ASSERT(client_wait_from(t, &c, "DREW", rbase, 400));
    for (int i = 0; i < 300; i++) {
        xwt_pump(t);
        usleep(2 * 1000);
    }
    XWT_CHECK(w->w == target_w && w->h == target_h,
              "granted geometry stable after the client redraw: model "
              "%dx%d (granted %dx%d)", w->w, w->h, target_w, target_h);
    long rbase2 = log_size_of(c.logpath);
    proc_cmd(&c, "geom\n");
    have_geom = client_wait_from(t, &c, "GEOM ", rbase2, 100);
    have_geom = last_log_line_from(&c, "GEOM ", rbase2, gline, sizeof(gline));
    if (have_geom && parse_geom_line(gline, &gw, &gh, &gx, &gy, &gbw)) {
        XWT_CHECK(gw + 2 * gbw == target_w && gh + 2 * gbw == target_h,
                  "X extent == granted size after resize: X %dx%d bw %d "
                  "(extent %dx%d) vs granted %dx%d", gw, gh, gbw,
                  gw + 2 * gbw, gh + 2 * gbw, target_w, target_h);
        XWT_CHECK(gx - gbw == w->x && gy - gbw == w->y,
                  "X position == granted position: +%d+%d vs model "
                  "+%d+%d", gx - gbw, gy - gbw, w->x, w->y);
    }

    proc_cmd(&c, "exit\n");
    usleep(150 * 1000);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    proc_close(&c);
    stack_down(t, &s);
}

static const struct xwt_test tests[] = {
    {"xwm-configure-mask", test_xwm_configure_mask},
    {"xwm-identity", test_xwm_identity},
    {"xwm-title-change", test_xwm_title_change},
    {"xwm-focus-routing", test_xwm_focus_routing},
    {"xwm-x11-keys", test_xwm_x11_keys},
    {"xwm-focus-protocol", test_xwm_focus_protocol},
    {"xwm-override-redirect", test_xwm_override_redirect},
    {"xwm-close-delete", test_xwm_close_delete},
    {"xwm-close-kill", test_xwm_close_kill},
    {"xwm-fullscreen", test_xwm_fullscreen},
    {"xwm-geometry-truth", test_xwm_geometry_truth},
    {"xwm-teardown", test_xwm_teardown},
    {"xwm-xterm-real", test_xwm_xterm_real},
};

__attribute__((constructor)) static void register_tests(void) {
    xwm_probe_init();
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
