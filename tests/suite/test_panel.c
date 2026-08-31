/* test_panel.c — M8 panel coverage.
 *
 * Two layers:
 *  1. client-library protocol tests: xwc_tasklist / xwc_wspaces driven
 *     against the live in-process compositor (announce, title, state,
 *     activate, close; workspace names/active/switch).
 *  2. the real xw-panel binary forked as a child process (like the
 *     exit-dialog tests): the bar maps on the top layer, its exclusive
 *     zone displaces windows, pointer clicks on the workspace buttons
 *     switch workspaces end-to-end, and the exit button speaks the
 *     session ctl protocol (a fake ctl socket in the test accepts the
 *     "exit-dialog" line).
 */
#include "xwtest.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------------------------------------------------------- shared */

static const char *panel_path(void) {
    if (access("build/bin/xw-panel", X_OK) == 0)
        return "build/bin/xw-panel";
    if (access("../build/bin/xw-panel", X_OK) == 0)
        return "../build/bin/xw-panel";
    return NULL;
}

static struct xw_layer_surface *first_top_layer(struct xwt_ctx *t) {
    struct xw_layer_surface *ls;
    if (wl_list_empty(&t->comp->wm->layers[2]))
        return NULL;
    ls = wl_container_of(t->comp->wm->layers[2].next, ls, link);
    return ls;
}

static uint32_t pixel_at(struct xwt_ctx *t, int x, int y) {
    int w = 0, h = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &w, &h);
    if (!pix || x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    return pix[y * w + x];
}

#define BAR_BG 0xff22262e

static bool bar_rendered(struct xwt_ctx *t) {
    return pixel_at(t, 640, 2) == BAR_BG || pixel_at(t, 640, 25) == BAR_BG;
}

/* fork the panel binary; returns pid or -1 */
static pid_t spawn_panel(struct xwt_ctx *t, const char *log_suffix) {
    const char *bin = panel_path();
    if (!bin)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        char path[128];
        snprintf(path, sizeof(path), "/tmp/xw-panel-child%s.log", log_suffix);
        int logfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(bin, "xw-panel", NULL);
        _exit(127);
    }
    return pid;
}

static void reap(int *pid) {
    if (*pid > 0) {
        kill(*pid, SIGKILL);
        waitpid(*pid, NULL, 0);
        *pid = 0;
    }
}

/* pump with wall-clock pacing while a separate client process
 * handshakes (session-3 lesson: forked children need real time) */
static void pump_ms(struct xwt_ctx *t, int ms) {
    for (int done = 0; done < ms; done += 10) {
        xwt_pump(t);
        usleep(10000);
    }
}

/* paced XWT_WAIT for conditions involving separate client processes:
 * yields the CPU so the child can exec + connect + handshake.
 * Returns true when the condition held. */
#define PANEL_WAIT(t, cond)                                                   \
    ({                                                                        \
        bool _ok = false;                                                     \
        for (int _ms = 0; _ms < 5000 && !(_ok = (cond)); _ms += 10) {          \
            xwt_pump(t);                                                      \
            usleep(10000);                                                    \
        }                                                                     \
        XWT_CHECK(_ok, "timeout (paced) waiting for: %s", #cond);             \
        _ok;                                                                  \
    })

/* ------------------------------------------------- client-library tests */

static int tl_changes;
static void tl_changed(void *ud) {
    (void)ud;
    tl_changes++;
}
static int ws_changes;
static void ws_changed(void *ud) {
    (void)ud;
    ws_changes++;
}

static void test_tasklist_client(struct xwt_ctx *t) {
    struct xwc_tasklist *tl =
        xwc_tasklist_create(&t->client, tl_changed, NULL);
    XWT_ASSERT(tl);
    XWT_CHECK(xwc_tasklist_first(tl) == NULL, "empty at start");
    xwc_sync(&t->client); /* manager bound + initial announce processed */

    /* two windows appear as tasks, with title + activation state */
    struct xwc_win *a = xwt_window_solid(t, 0xff112233, 200, 100, "Alpha");
    struct xwc_win *b = xwt_window_solid(t, 0xff445566, 200, 100, "Beta");
    XWT_ASSERT(a && b);
    XWT_WAIT(t, ({
        int n = 0;
        for (struct xwc_task *k = xwc_tasklist_first(tl); k;
             k = xwc_task_next(k))
            n++;
        n == 2;
    }));
    bool have_alpha = false, have_beta = false;
    for (struct xwc_task *k = xwc_tasklist_first(tl); k;
         k = xwc_task_next(k)) {
        if (strcmp(xwc_task_title(k), "Alpha") == 0)
            have_alpha = true;
        if (strcmp(xwc_task_title(k), "Beta") == 0)
            have_beta = true;
    }
    XWT_CHECK(have_alpha && have_beta, "titles tracked");

    /* activation: the most recently mapped window (Beta) holds focus;
     * the tasklist mirrors it (and Alpha was deactivated) */
    bool beta_active = false, alpha_active = false;
    XWT_WAIT(t, ({
        beta_active = alpha_active = false;
        for (struct xwc_task *k = xwc_tasklist_first(tl); k;
             k = xwc_task_next(k)) {
            if (strcmp(xwc_task_title(k), "Beta") == 0 && xwc_task_active(k))
                beta_active = true;
            if (strcmp(xwc_task_title(k), "Alpha") == 0 &&
                xwc_task_active(k))
                alpha_active = true;
        }
        beta_active;
    }));
    XWT_CHECK(beta_active, "active flag follows focus (Beta)");
    XWT_CHECK(!alpha_active, "deactivated task flagged inactive");

    /* client-side activate() focuses Alpha server-side */
    for (struct xwc_task *k = xwc_tasklist_first(tl); k;
         k = xwc_task_next(k)) {
        if (strcmp(xwc_task_title(k), "Alpha") == 0) {
            xwc_tasklist_activate(tl, k);
            break;
        }
    }
    XWT_WAIT(t, t->comp->wm->focused &&
                    strcmp(t->comp->wm->focused->title, "Alpha") == 0);
    XWT_CHECK(true, "activate() focuses the window");

    /* client-side close() asks Alpha to close: the server sends the
     * xdg_toplevel close event; the window decides (here: the test
     * destroys it), then the task disappears via the closed event */
    for (struct xwc_task *k = xwc_tasklist_first(tl); k;
         k = xwc_task_next(k)) {
        if (strcmp(xwc_task_title(k), "Alpha") == 0) {
            xwc_tasklist_close(tl, k);
            break;
        }
    }
    XWT_WAIT(t, xwc_win_closed(a));
    XWT_CHECK(true, "close() request reached the window");
    xwc_win_destroy(a);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    XWT_WAIT(t, ({
        int n = 0;
        for (struct xwc_task *k = xwc_tasklist_first(tl); k;
             k = xwc_task_next(k))
            n++;
        n == 1;
    }));
    XWT_CHECK(true, "closed task removed from the list");

    xwc_tasklist_destroy(tl);
    xwc_win_destroy(b);
}

static void test_workspaces_client(struct xwt_ctx *t) {
    struct xwc_wspaces *wl = xwc_wspaces_create(&t->client, ws_changed, NULL);
    XWT_ASSERT(wl);
    XWT_WAIT(t, ws_changes > 0);
    XWT_CHECK(xwc_wspaces_count(wl) == t->comp->wm->ws_count,
              "workspace count mirrors the wm (%d vs %d)",
              xwc_wspaces_count(wl), t->comp->wm->ws_count);
    XWT_CHECK(strcmp(xwc_wspaces_name(wl, 0), "Workspace 1") == 0,
              "workspace 1 name");
    XWT_CHECK(xwc_wspaces_active(wl, 0) && !xwc_wspaces_active(wl, 1),
              "workspace 1 active initially");

    int before = ws_changes;
    xwc_wspaces_activate(wl, 2);
    XWT_WAIT(t, ws_changes > before);
    XWT_CHECK(t->comp->wm->ws_current == 2, "activate() switched the wm");
    XWT_CHECK(xwc_wspaces_active(wl, 2) && !xwc_wspaces_active(wl, 0),
              "active state follows the switch");

    xwc_wspaces_destroy(wl);
}

/* ------------------------------------------------- panel process tests */

static void test_panel_maps(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-maps");
    XWT_ASSERT(pid > 0);

    /* the bar maps on the top layer and renders */
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);
    XWT_CHECK(true, "panel layer mapped (top)");
    XWT_CHECK(first_top_layer(t)->h == 28, "bar height 28");
    PANEL_WAIT(t, bar_rendered(t));
    XWT_CHECK(bar_rendered(t), "bar pixels rendered");

    /* the exclusive zone displaces windows below the bar */
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->exclusive_zone == 28);
    struct xw_output *o =
        wl_container_of(t->comp->outputs.next, o, link);
    XWT_CHECK(o->usable.y >= 28, "usable area starts below the bar (y=%d)",
              o->usable.y);

    /* a new window lands below the bar */
    struct xwc_win *win = xwt_window_solid(t, 0xff778899, 100, 80, "Below");
    XWT_ASSERT(win);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_window *w = NULL;
    wl_list_for_each(w, &t->comp->wm->windows, link) {
        if (strcmp(w->title, "Below") == 0)
            break;
    }
    XWT_CHECK(w && w->y >= 28, "window placed below the bar (y=%d)",
              w ? w->y : -1);

    reap(&pid);
    xwc_win_destroy(win);
}

static void test_panel_clicks(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-clicks");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* deterministic switcher geometry: launcher at x=3..39, workspace
     * buttons 24 wide with 3px gaps starting at x=42 → ws2 at 69..93 */
    int ws2_x = 42 + 27 + 12;
    XWT_CHECK(t->comp->wm->ws_current == 0, "start on workspace 1");
    xw_compositor_inject_pointer_motion(t->comp, ws2_x, 14);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, t->comp->wm->ws_current == 1);
    XWT_CHECK(t->comp->wm->ws_current == 1,
              "click on switcher button 2 switched workspaces (%d)",
              t->comp->wm->ws_current);

    /* clicking the active button again is harmless; clicking ws3 works
     * from the new state */
    xw_compositor_inject_pointer_motion(t->comp, 42 + 2 * 27 + 12, 14);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, t->comp->wm->ws_current == 2);
    XWT_CHECK(t->comp->wm->ws_current == 2, "third workspace switch works");

    reap(&pid);
}

static void test_panel_exit_button(struct xwt_ctx *t) {
    /* a fake session manager: accepts one connection, asserts the
     * "exit-dialog" line, replies ok */
    char ctl_path[192];
    snprintf(ctl_path, sizeof(ctl_path), "%s/xw-session.sock",
             g_runtimedir());
    unlink(ctl_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    int ncpy = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ctl_path);
    XWT_ASSERT(ncpy >= 0 && (size_t)ncpy < sizeof(addr.sun_path));
    XWT_ASSERT(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    XWT_ASSERT(listen(lfd, 4) == 0);

    pid_t pid = spawn_panel(t, "-exit");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* find the red exit button by scanning the bar for its fill color */
    int red_x = -1;
    for (int i = 0; i < 2000 && red_x < 0; i++) {
        xwt_pump(t);
        for (int x = 800; x < 1280; x++) {
            if (pixel_at(t, x, 14) == 0xffa33434) {
                red_x = x;
                break;
            }
        }
        if (red_x < 0)
            usleep(10000);
    }
    XWT_ASSERT(red_x > 0); /* exit button rendered */
    int exit_x = red_x + 10; /* inside the button */

    xw_compositor_inject_pointer_motion(t->comp, exit_x, 14);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);

    /* service the fake ctl socket while the panel connects + writes;
     * the panel child blocks in read() until the reply, so keep the
     * in-process server pumping while we service its ctl connection */
    char line[128] = {0};
    bool got = false;
    for (int i = 0; i < 600 && !got; i++) {
        xwt_pump(t);
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        if (poll(&pfd, 1, 0) == 1) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                ssize_t n = read(cfd, line, sizeof(line) - 1);
                if (n > 0) {
                    line[n] = 0;
                    got = true;
                    dprintf(cfd, "ok exit dialog spawned\n");
                }
                close(cfd);
            }
        }
        usleep(10000);
    }
    XWT_CHECK(got && strncmp(line, "exit-dialog", 11) == 0,
              "exit button sent the ctl line (got '%s')", got ? line : "(none)");

    /* the panel keeps running after the action */
    pump_ms(t, 200);
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after exit click");

    reap(&pid);
    unlink(ctl_path);
    close(lfd);
}

/* the launcher + full session wiring (ctl run, real exit dialog spawn)
 * are covered process-level by scripts/test-session.sh */

static const struct xwt_test tests[] = {
    {"tasklist-client", test_tasklist_client},
    {"workspace-client", test_workspaces_client},
    {"panel-maps", test_panel_maps},
    {"panel-clicks", test_panel_clicks},
    {"panel-exit-button", test_panel_exit_button},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
