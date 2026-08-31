/* test_session.c — session-exit integration: the graphical exit dialog
 * as a real child process against the in-process compositor, driven by
 * keyboard (Esc = cancel) and the ctl-socket protocol of xw-session.
 */
#include "xwtest.h"
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

/* locate the xw-exit binary (build/bin layout, run from repo root or
 * from the build dir) */
static const char *exit_dialog_path(void) {
    if (access("build/bin/xw-exit", X_OK) == 0)
        return "build/bin/xw-exit";
    if (access("../build/bin/xw-exit", X_OK) == 0)
        return "../build/bin/xw-exit";
    if (access("./xw-exit", X_OK) == 0)
        return "./xw-exit";
    return NULL;
}

/* run xw-exit as a child process; returns its exit code. Escape can be
 * injected after start_ms of pumping. */
static int run_exit_dialog(struct xwt_ctx *t, int start_ms, bool send_escape,
                           int timeout_ms) {
    const char *bin = exit_dialog_path();
    if (!bin)
        return 127;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        int logfd = open("/tmp/xw-exit-child.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(bin, "xw-exit", NULL);
        _exit(127);
    }
    int status = 0;
    bool escaped = false;
    for (int ms = 0; ms < timeout_ms; ms += 20) {
        xwt_pump(t);
        if (send_escape && !escaped && ms >= start_ms) {
            xw_compositor_inject_key(t->comp, K_ESC, true);
            xwt_pump(t);
            xw_compositor_inject_key(t->comp, K_ESC, false);
            escaped = true;
        }
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -3; /* timeout */
}

static void test_exit_dialog_cancel(struct xwt_ctx *t) {
    /* the exit dialog maps a modal overlay; Escape cancels with rc 0 */
    int rc = run_exit_dialog(t, 1500, true, 15000);
    if (rc == 127) {
        XWT_CHECK(false, "xw-exit binary not found (run from repo root)");
        return;
    }
    XWT_CHECK(rc == 0, "exit dialog cancel rc=%d (want 0)", rc);
}

static void test_exit_dialog_rendered(struct xwt_ctx *t) {
    /* the dialog is a layer-shell overlay covering the output: after
     * mapping, pixels change away from the plain background */
    const char *bin = exit_dialog_path();
    if (!bin) {
        XWT_CHECK(false, "xw-exit binary not found");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        int logfd = open("/tmp/xw-exit-child.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(bin, "xw-exit", NULL);
        _exit(127);
    }
    /* pump until the overlay commits (output pixels change) */
    int pw = 0, ph = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    XWT_ASSERT(pix);
    uint32_t before = pix[ph / 2 * pw + pw / 2];
    bool changed = false;
    for (int i = 0; i < 400 && !changed; i++) {
        xwt_pump(t);
        pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
        changed = (pix[ph / 2 * pw + pw / 2] != before) ||
                  (pix[10 * pw + 10] != before);
        /* the dialog is a separate process: exec + connect + layer
         * handshake need real wall-clock time, so pace the loop */
        if (!changed)
            usleep(10000);
    }
    XWT_CHECK(changed, "overlay pixels rendered");
    /* overlay layer surfaces exist */
    int n_layers = 0;
    for (int l = 0; l < 4; l++) {
        struct xw_layer_surface *ls;
        wl_list_for_each(ls, &t->comp->wm->layers[l], link) n_layers++;
    }
    XWT_CHECK(n_layers == 1, "one layer surface mapped, got %d", n_layers);

    /* escape: deliver keyboard to the overlay (exclusive keyboard) */
    xw_compositor_inject_key(t->comp, K_ESC, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_ESC, false);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        xwt_pump(t);
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

/* the session ctl protocol is covered end-to-end by
 * scripts/test-session.sh (process level: xw-session + real
 * compositor + ctl commands + clean logout). */

static const struct xwt_test tests[] = {
    {"exit-dialog-cancel", test_exit_dialog_cancel},
    {"exit-dialog-rendered", test_exit_dialog_rendered},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
