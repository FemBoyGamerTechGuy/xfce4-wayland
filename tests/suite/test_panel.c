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
#include <sys/stat.h>
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
        setenv("XW_PANEL_TRACE", "1", 1);
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


/* xw-workspace-info-v1: every task is annotated with its workspace,
 * moves arrive as events, sticky reports -1, and the annotation dies
 * cleanly with the toplevel (the UAF guard for the pager's data). */
static void test_tasklist_workspace(struct xwt_ctx *t) {
    /* window mapped BEFORE the tasklist exists: the bind announces it
     * and the annotation arrives right after */
    struct xwc_win *a = xwt_window_solid(t, 0xff123456, 200, 100, "WsA");
    XWT_ASSERT(a);

    struct xwc_tasklist *tl =
        xwc_tasklist_create(&t->client, tl_changed, NULL);
    XWT_ASSERT(tl);
    xwc_sync(&t->client);
    XWT_WAIT(t, xwc_tasklist_first(tl) != NULL);
    struct xwc_task *ka = xwc_tasklist_first(tl);
    XWT_CHECK(strcmp(xwc_task_title(ka), "WsA") == 0, "tasklist sees WsA");

    /* initial annotation = the window's current workspace (0) */
    XWT_WAIT(t, xwc_task_workspace(ka) == 0);
    XWT_CHECK(xwc_task_workspace(ka) == 0,
              "initial workspace annotation is 0 (%d)",
              xwc_task_workspace(ka));

    /* server-side move: the event must reach the client */
    struct xw_window *wa = t->comp->wm->focused;
    XWT_ASSERT(wa && strcmp(wa->title, "WsA") == 0);
    xw_wm_window_to_workspace(t->comp->wm, wa, 1);
    XWT_WAIT(t, xwc_task_workspace(ka) == 1);
    XWT_CHECK(xwc_task_workspace(ka) == 1, "move to ws 1 propagated (%d)",
              xwc_task_workspace(ka));

    /* sticky (-1) is pushed the same way */
    wa->ws = -1;
    xw_workspace_info_notify(t->comp, wa);
    XWT_WAIT(t, xwc_task_workspace(ka) == -1);
    XWT_CHECK(xwc_task_workspace(ka) == -1, "sticky reports -1");

    /* a window mapped AFTER the tasklist binds is annotated too */
    xw_wm_window_to_workspace(t->comp->wm, wa, 0);
    struct xwc_win *b = xwt_window_solid(t, 0xff654321, 200, 100, "WsB");
    XWT_ASSERT(b);
    struct xwc_task *kb = NULL;
    XWT_WAIT(t, (kb = xwc_task_next(xwc_tasklist_first(tl))) != NULL);
    XWT_CHECK(strcmp(xwc_task_title(kb), "WsB") == 0, "late task tracked");
    XWT_WAIT(t, xwc_task_workspace(kb) == 0);
    XWT_CHECK(xwc_task_workspace(kb) == 0,
              "late window annotated with its ws (%d)",
              xwc_task_workspace(kb));

    /* window gone: the toplevel closed event removes the task; the
     * annotation must not outlive it (ASan watches this) */
    xwc_win_destroy(a);
    XWT_WAIT(t, xwc_tasklist_first(tl) && xwc_task_next(xwc_tasklist_first(tl)) == NULL);
    XWT_CHECK(true, "annotation released with the closed toplevel");
    xwc_win_destroy(b);
    XWT_WAIT(t, xwc_tasklist_first(tl) == NULL);

    /* keep the connection alive well past the closed events: the
     * client's handle destructor requests must not kill the link (the
     * "invalid object" regression — server-side destroy of a
     * server-range id vs the client's legitimate destroy) */
    for (int i = 0; i < 50 && !t->client_dead; i++) {
        xwt_pump(t);
        usleep(10000);
    }
    XWT_CHECK(!t->client_dead, "connection survives window teardown");
    xwc_tasklist_destroy(tl);
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


/* -------------------------------------------------- bar widget finder */
/* Geometry-agnostic widget location for the process-level tests: scan
 * the bar's middle row for contiguous non-background runs (buttons).
 * The runs classify left-to-right as: start, launchers?, task
 * buttons?, workspace boxes, clock, exit — the panel's region order.
 * Survives any change of pixel metrics. */
#define MAX_RUNS 32
struct bar_run {
    int x0, x1;
    uint32_t color;
};
static int bar_scan_runs(struct xwt_ctx *t, struct bar_run *runs) {
    int h = first_top_layer(t) ? first_top_layer(t)->h : 30;
    int y = h / 2;
    int n = 0;
    int x = 0;
    const int W = 1280;
    while (x < W && n < MAX_RUNS) {
        uint32_t c = pixel_at(t, x, y);
        if (c == BAR_BG || c == 0) { /* background or unmapped */
            x++;
            continue;
        }
        int x0 = x;
        while (x < W && pixel_at(t, x, y) != BAR_BG &&
               pixel_at(t, x, y) != 0)
            x++;
        if (x - x0 < 6) /* sub-6px runs are glyph/noise artifacts */
            continue;
        runs[n].x0 = x0;
        runs[n].x1 = x;
        runs[n].color = c;
        n++;
    }
    return n;
}

/* center x of run i (from the left) */
static int bar_run_cx(struct xwt_ctx *t, int i) {
    struct bar_run runs[MAX_RUNS];
    int n = bar_scan_runs(t, runs);
    if (i < 0 || i >= n)
        return -1;
    return (runs[i].x0 + runs[i].x1) / 2;
}
/* wait for the bar to render, then the Start button center (run 0) */
static int start_cx(struct xwt_ctx *t) {
    for (int i = 0; i < 300; i++) {
        xwt_pump(t);
        int cx = bar_run_cx(t, 0);
        if (cx > 0)
            return cx;
        usleep(10000);
    }
    return -1;
}
#define START_CX start_cx(t)

/* the workspace pager boxes: runs between the tasklist area and the
 * clock/exit pair at the right end. With no windows open the run list
 * is [start, ws..., clock, exit] — the ws runs are 1..n-3. */
static int ws_box_cx(struct xwt_ctx *t, int ws_idx) {
    struct bar_run runs[MAX_RUNS];
    int n = bar_scan_runs(t, runs);
    /* find the exit (red) run at the right, clock is one before it */
    int exit_i = -1;
    for (int i = n - 1; i >= 0; i--)
        if (runs[i].color == 0xffa33434 || runs[i].color == 0xffc94b4b) {
            exit_i = i;
            break;
        }
    if (exit_i < 2)
        return -1;
    int clock_i = exit_i - 1;
    /* the boxes run left from the clock: count = clock_i - 1 - 0... */
    int first_ws = 1; /* run 0 = the start button (no launchers/tasks) */
    int n_ws = clock_i - first_ws;
    if (ws_idx < 0 || ws_idx >= n_ws)
        return -1;
    struct bar_run *r = &runs[first_ws + ws_idx];
    return (r->x0 + r->x1) / 2;
}

/* ------------------------------------------------- panel process tests */

static void test_panel_maps(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-maps");
    XWT_ASSERT(pid > 0);

    /* the bar maps on the top layer and renders */
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);
    XWT_CHECK(true, "panel layer mapped (top)");
    XWT_CHECK(first_top_layer(t)->h == 30,
              "bar height auto-derived at 720p (got %d)",
              first_top_layer(t)->h);
    PANEL_WAIT(t, bar_rendered(t));
    XWT_CHECK(bar_rendered(t), "bar pixels rendered");

    /* the exclusive zone displaces windows below the bar */
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->exclusive_zone == 30);
    struct xw_output *o =
        wl_container_of(t->comp->outputs.next, o, link);
    XWT_CHECK(o->usable.y >= 30, "usable area starts below the bar (y=%d)",
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
    XWT_CHECK(w && w->y >= 30, "window placed below the bar (y=%d)",
              w ? w->y : -1);

    reap(&pid);
    xwc_win_destroy(win);
}

static void test_panel_clicks(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-clicks");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* the workspace boxes live in the right region now; locate them
     * by pixel scan (geometry-agnostic) */
    int ws2_x = -1;
    for (int i = 0; i < 300 && ws2_x < 0; i++) {
        xwt_pump(t);
        ws2_x = ws_box_cx(t, 1);
        if (ws2_x < 0)
            usleep(10000);
    }
    XWT_ASSERT(ws2_x > 0);
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
    int ws3_x = ws_box_cx(t, 2);
    XWT_ASSERT(ws3_x > 0);
    xw_compositor_inject_pointer_motion(t->comp, ws3_x, 14);
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

/* --------------------------------------------------- pointer focus tests */

/* the panel-interaction lifecycle: enter, leave, hover redraw, the
 * focus pointer itself, and — the UAF regression — focus surviving the
 * panel process dying while hovered (surface destroyed under focus) */
static void test_panel_pointer_focus(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-focus");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    struct xw_surface *panel_surf = first_top_layer(t)->surface;

    /* pointer over the bar takes focus on the panel surface */
    xw_compositor_inject_pointer_motion(t->comp, 640, 14);
    XWT_WAIT(t, seat->ptr_focus == panel_surf);
    XWT_CHECK(seat->ptr_focus == panel_surf,
              "pointer over bar focuses the panel surface");

    /* leaving the bar drops focus (no window below on empty desktop) */
    xw_compositor_inject_pointer_motion(t->comp, 640, 400);
    XWT_WAIT(t, seat->ptr_focus == NULL);
    XWT_CHECK(seat->ptr_focus == NULL, "leaving the bar clears focus");

    /* re-entering restores it */
    xw_compositor_inject_pointer_motion(t->comp, 640, 14);
    XWT_WAIT(t, seat->ptr_focus == panel_surf);
    XWT_CHECK(true, "re-entering the bar re-focuses");

    /* hover feedback proves enter+motion reach the panel CLIENT and it
     * redraws: the workspace button lights up its hover fill */
    xw_compositor_inject_pointer_motion(t->comp, 50, 14);
    /* NOTE: check a pixel INSIDE the button but clear of the software
     * cursor — the cursor itself is drawn over the hover fill at the
     * exact pointer position */
    PANEL_WAIT(t, pixel_at(t, 44, 14) == 0xff3b4252);
    XWT_CHECK(pixel_at(t, 44, 14) == 0xff3b4252,
              "workspace button shows the hover fill (enter+motion+redraw)");

    /* THE regression: panel dies while it holds pointer focus. The
     * surface destroy path must clear ptr_focus BEFORE freeing it — a
     * stale pointer here is a use-after-free on the next motion and
     * poisons every later focus decision (frozen-input decay). */
    XWT_ASSERT(seat->ptr_focus == panel_surf);
    reap(&pid);
    XWT_WAIT(t, seat->ptr_focus == NULL);
    XWT_CHECK(seat->ptr_focus == NULL,
              "panel death under the cursor clears pointer focus (no UAF)");
    /* the next motion re-evaluates cleanly against freed memory gone */
    xw_compositor_inject_pointer_motion(t->comp, 700, 16);
    xwt_pump(t);
    xw_compositor_inject_pointer_motion(t->comp, 640, 400);
    xwt_pump(t);
    XWT_CHECK(true, "motion after panel death does not crash");
}

/* late wl_pointer creation must receive the enter replay: a client
 * that binds wl_seat but creates its wl_pointer AFTER its surface
 * already holds pointer focus used to get motion events without ever
 * seeing enter (clients gate all pointer state on enter) */
static int g_late_enter_count;
static struct wl_surface *g_late_enter_surface;
static void late_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                           struct wl_surface *surface, wl_fixed_t sx,
                           wl_fixed_t sy) {
    (void)data;
    (void)p;
    (void)serial;
    (void)sx;
    (void)sy;
    g_late_enter_count++;
    g_late_enter_surface = surface;
}
static void late_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                           struct wl_surface *surface) {
    (void)data;
    (void)p;
    (void)serial;
    (void)surface;
}
static void late_ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                            wl_fixed_t sx, wl_fixed_t sy) {
    (void)data;
    (void)p;
    (void)time;
    (void)sx;
    (void)sy;
}
static void late_ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)data;
    (void)p;
    (void)serial;
    (void)time;
    (void)button;
    (void)state;
}
static void late_ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)p;
    (void)time;
    (void)axis;
    (void)value;
}
static void late_ptr_frame(void *data, struct wl_pointer *p) {
    (void)data;
    (void)p;
}
static const struct wl_pointer_listener late_ptr_listener = {
    .enter = late_ptr_enter,
    .leave = late_ptr_leave,
    .motion = late_ptr_motion,
    .button = late_ptr_button,
    .axis = late_ptr_axis,
    .frame = late_ptr_frame,
};

static void test_late_pointer_enter_replay(struct xwt_ctx *t) {
    g_late_enter_count = 0;
    g_late_enter_surface = NULL;

    struct xwc_win *win = xwt_window_solid(t, 0xff334455, 300, 200, "Hovered");
    XWT_ASSERT(win);
    XWT_WAIT(t, t->comp->wm->focused && t->comp->wm->focused->title &&
             strcmp(t->comp->wm->focused->title, "Hovered") == 0);

    /* focus the window with the pointer (cursor 640,400 -> 200x200 at
     * center placement: map puts windows at the usable-area center) */
    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    int wx = t->comp->wm->focused->x, wy = t->comp->wm->focused->y;
    xw_compositor_inject_pointer_motion(t->comp, wx + 100, wy + 100);
    XWT_WAIT(t, seat->ptr_focus &&
                    seat->ptr_focus->role == XW_SURFACE_ROLE_XDG_TOPLEVEL);
    XWT_CHECK(seat->ptr_focus == t->comp->wm->focused->surface,
              "window focused under the cursor");

    /* destroy the client's wl_pointer (simulating a client that creates
     * it late), then re-create it on the same seat */
    wl_pointer_destroy(t->client.pointer);
    t->client.pointer = NULL;
    t->client.pointer = wl_seat_get_pointer(t->client.seat);
    XWT_ASSERT(t->client.pointer);
    wl_pointer_add_listener(t->client.pointer, &late_ptr_listener, NULL);

    XWT_WAIT(t, g_late_enter_count > 0);
    XWT_CHECK(g_late_enter_count == 1,
              "late wl_pointer received exactly one enter replay (%d)",
              g_late_enter_count);
    XWT_CHECK(g_late_enter_surface &&
                  wl_proxy_get_id((struct wl_proxy *)g_late_enter_surface) ==
                      wl_resource_get_id(t->comp->wm->focused->surface->res),
              "enter replay names the focused surface");

    xwc_win_destroy(win);
}

/* the launcher button: resolves a terminal ($XW_TERMINAL wins) and
 * sends the ctl 'run <terminal>' line to the session manager */
static void test_panel_launcher(struct xwt_ctx *t) {
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

    /* no applications anywhere: the Start button's fallback is the
     * resolved terminal (the pre-menu launcher behavior) */
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/noapps-%d", g_runtimedir(),
             (int)getpid());
    mkdir(empty, 0755);
    setenv("XDG_DATA_HOME", empty, 1);
    setenv("XDG_DATA_DIRS", empty, 1);
    setenv("XW_TERMINAL", "/bin/true", 1); /* inherited by the panel */
    pid_t pid = spawn_panel(t, "-launcher");
    unsetenv("XW_TERMINAL");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* Start button occupies the bar's left edge; its center is around
     * x=20 (icon + label; the run scan locates it precisely) */
    xw_compositor_inject_pointer_motion(t->comp, 20, 15);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);

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
                    dprintf(cfd, "ok spawned\n");
                }
                close(cfd);
            }
        }
        usleep(10000);
    }
    XWT_CHECK(got && strncmp(line, "run /bin/true", 13) == 0,
              "launcher sent the resolved terminal (got '%.60s')",
              got ? line : "(none)");

    pump_ms(t, 100);
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after launcher click");
    reap(&pid);
    unlink(ctl_path);
    close(lfd);
}

/* ------------------------------------------------------------------ */
/* Start-button robustness: the user report behind this suite —
 * "Start does nothing; clicking it repeatedly crashes xw-panel".
 * With the applications menu, repeated Start activation is a rapid
 * popup create/destroy cycle (menu open -> Start toggle closes it);
 * it must be idempotent and must never take the panel down. */

/* service one pending fake-session-manager ctl connection: read the
 * line, reply ok (used by the menu-launch and exit-dialog checks) */
static void handled_ctl_line(int lfd, char *line, size_t line_n, bool *got) {
    if (*got)
        return;
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    if (poll(&pfd, 1, 0) != 1)
        return;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return;
    ssize_t n = read(cfd, line, line_n - 1);
    if (n > 0) {
        line[n] = 0;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        dprintf(cfd, "ok spawned\n");
        *got = true;
    }
    shutdown(cfd, SHUT_WR);
    close(cfd);
}

static void fake_appdir(char *out, size_t outn) {
    /* XDG_DATA_HOME points at out; entries live in out/applications */
    snprintf(out, outn, "%s/apps-%d", g_runtimedir(), (int)getpid());
    mkdir(out, 0755); /* parent first: mkdir is not recursive */
    char appsub[340];
    snprintf(appsub, sizeof(appsub), "%.320s/applications", out);
    mkdir(appsub, 0755);
    static const char *const entries[] = {
        "[Desktop Entry]\nType=Application\nName=Alpha\nExec=/bin/alpha %f\n",
        "[Desktop Entry]\nType=Application\nName=Beta\nExec=beta --flag\n",
        "[Desktop Entry]\nType=Application\nName=Gamma\nExec=gamma\n",
        /* filtered out: NoDisplay */
        "[Desktop Entry]\nType=Application\nName=Hidden App\n"
        "Exec=/bin/hidden\nNoDisplay=true\n",
        /* filtered out: wrong type */
        "[Desktop Entry]\nType=Link\nName=Web\nURL=https://xw\n",
    };
    static const char *const files[] = {"alpha.desktop", "beta.desktop",
                                        "gamma.desktop", "hidden.desktop",
                                        "web.desktop"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", appsub, files[i]);
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(entries[i], f);
            fclose(f);
        }
    }
    /* an empty extra data dir: XDG_DATA_DIRS points here so the scan
     * never sees the real system applications (deterministic items) */
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/emptydata-%d", g_runtimedir(),
             (int)getpid());
    mkdir(empty, 0755);
}

static int n_top_popups(struct xwt_ctx *t) {
    int n = 0;
    struct xw_popup *p;
    wl_list_for_each(p, &t->comp->popups, link)
        n++;
    return n;
}

static struct xw_popup *the_menu(struct xwt_ctx *t) {
    if (wl_list_empty(&t->comp->popups))
        return NULL;
    struct xw_popup *p;
    p = wl_container_of(t->comp->popups.next, p, link);
    return p;
}

static void test_panel_start_repeated(struct xwt_ctx *t) {
    char appdir[300];
    fake_appdir(appdir, sizeof(appdir));
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/emptydata-%d", g_runtimedir(),
             (int)getpid());
    setenv("XDG_DATA_HOME", appdir, 1);
    setenv("XDG_DATA_DIRS", empty, 1);

    pid_t pid = spawn_panel(t, "-start-rep");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* 20 paced open/close cycles (the 600ms pacing clears the
     * same-click dismissal-suppression window, so every odd click
     * opens and every even click toggles closed) */
    bool saw_popup = false;
    for (int round = 0; round < 20; round++) {
        xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
        pump_ms(t, 60);
        xw_compositor_inject_pointer_button(t->comp, 0x110, true);
        xwt_pump(t);
        xw_compositor_inject_pointer_button(t->comp, 0x110, false);
        /* let the panel run its popup create (blocking sync against
         * this server) or destroy */
        pump_ms(t, 600);
        if (n_top_popups(t) > 0)
            saw_popup = true;
        if (kill(pid, 0) != 0) {
            XWT_CHECK(false, "panel died during round %d of 20", round);
            break;
        }
    }
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after 20 Start toggle cycles");
    XWT_CHECK(saw_popup, "the menu opened at least once");

    /* and a rapid-fire burst on top: clicks faster than the
     * suppression window must not thrash or crash either */
    for (int round = 0; round < 30; round++) {
        xw_compositor_inject_pointer_button(t->comp, 0x110, true);
        xwt_pump(t);
        xw_compositor_inject_pointer_button(t->comp, 0x110, false);
        pump_ms(t, 40);
    }
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after a 30-click burst");

    reap(&pid);
}

/* the applications menu end to end: open (popup parented to the bar
 * layer, anchored under the Start button), Escape dismissal, outside
 * click dismissal, item launch through the ctl wire, toggle, and the
 * destroy-under-cursor UAF guard */
static void test_panel_menu(struct xwt_ctx *t) {
    char appdir[300];
    fake_appdir(appdir, sizeof(appdir));
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/emptydata-%d", g_runtimedir(),
             (int)getpid());
    setenv("XDG_DATA_HOME", appdir, 1);
    setenv("XDG_DATA_DIRS", empty, 1);

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
    XWT_ASSERT(listen(lfd, 8) == 0);

    pid_t pid = spawn_panel(t, "-menu");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);

    /* 1. Start click opens the menu: popup exists, mapped, parented to
     * the bar layer, anchored under the button */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, the_menu(t) && the_menu(t)->mapped);
    struct xw_popup *pu = the_menu(t);
    XWT_ASSERT(pu);
    XWT_CHECK(pu->mapped, "menu popup mapped");
    XWT_CHECK(pu->parent == first_top_layer(t)->surface,
              "menu parented to the bar layer surface");
    XWT_CHECK(pu->anchor_x == 3 && pu->anchor_y == 30,
              "menu anchored under the Start button (3,%d)", pu->anchor_y);
    /* two-pane menu: categories (All + Other for the un-categorized
     * fixture) left, applications right; rows = cats + 2, header row,
     * 2px pads; width = 2 + 176 + 336 + 2 */
    const int ROW = XWC_LINE_H + 10;
    XWT_CHECK(pu->h == 4 + ROW + 4 * ROW, "menu height (%d)", pu->h);
    XWT_CHECK(pu->w == 516, "menu width 516 (%d)", pu->w);
    /* the app pane starts at x = 2 + 176 inside the popup; item 0's
     * row is below the search header (popup y=30 + 2 + 29 + 14) */
    const int APP0_X = 3 + 2 + 176 + 60;
    const int APP0_Y = 30 + 2 + 29 + 14;
    PANEL_WAIT(t, pixel_at(t, 500, 30 + 2 + 29 + 14) == 0xff2b313b);
    XWT_CHECK(pixel_at(t, 500, 30 + 2 + 29 + 14) == 0xff2b313b,
              "app pane background rendered");

    /* 2. hover: motion over item 0 lights the highlight (checked
     * off-axis: the software cursor sprite covers the pointer pixel) */
    xw_compositor_inject_pointer_motion(t->comp, APP0_X, APP0_Y);
    PANEL_WAIT(t, pixel_at(t, APP0_X + 60, APP0_Y) == 0xff3584e4);
    XWT_CHECK(pixel_at(t, APP0_X + 60, APP0_Y) == 0xff3584e4,
              "menu item hover highlight rendered");
    XWT_CHECK(seat->grab_surface == pu->surface,
              "the menu holds the seat grab (all pointer events)");

    /* 3. Escape dismisses (the popup owns keyboard focus via grab) */
    xw_compositor_inject_key(t->comp, K_ESC, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_ESC, false);
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "Escape closed the menu");
    /* pointer was ON the menu when it died: the destroyed surface must
     * not keep pointer or grab focus, and further motion must be clean
     * (the UAF guard) */
    XWT_WAIT(t, seat->ptr_focus == NULL && seat->grab_surface == NULL);
    xw_compositor_inject_pointer_motion(t->comp, 700, 300);
    xwt_pump(t);
    XWT_CHECK(true, "motion after menu death under the cursor is clean");
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after Escape close");

    /* 4. reopen + item click launches through the ctl wire */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300); /* clear the suppression window */
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    /* item 0 (Alpha, sorted): click its center; the panel forks the
     * async ctl round trip — service the fake socket for it */
    xw_compositor_inject_pointer_motion(t->comp, APP0_X, APP0_Y);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);

    char line[128] = {0};
    bool got = false;
    for (int i = 0; i < 400 && !got; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line, sizeof(line), &got);
        usleep(10000);
    }
    XWT_CHECK(got && strncmp(line, "run /bin/alpha", 14) == 0,
              "menu item launched via ctl (got '%s')", got ? line : "(none)");
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "menu closed after item selection");
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after launching an item");

    /* 4b. search: typing filters the app pane; Enter launches the
     * first hit; the fixture apps are Alpha/Beta/Gamma */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    PANEL_WAIT(t, seat && seat->grab_surface == the_menu(t)->surface);
    XWT_CHECK(true, "menu holds the seat grab before typing");
    /* inject_key takes raw linux keycodes (K_* style): letters from
     * the evdev code space */
    static const int KEY_OF_LETTER[26] = {
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24,
        25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    const char *word = "beta";
    for (const char *c = word; *c; c++) {
        uint32_t code = KEY_OF_LETTER[*c - 'a'];
        xw_compositor_inject_key(t->comp, code, true);
        xwt_pump(t);
        xw_compositor_inject_key(t->comp, code, false);
        pump_ms(t, 40);
    }
    /* only Beta matches: hover lands on row 0 of the filtered pane */
    xw_compositor_inject_key(t->comp, 28 /* KEY_ENTER */, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, 28, false);
    char line3[128] = {0};
    bool got3 = false;
    for (int i = 0; i < 400 && !got3; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line3, sizeof(line3), &got3);
        usleep(10000);
    }
    XWT_CHECK(got3 && strcmp(line3, "run beta --flag") == 0,
              "search + Enter launches the filtered item (got '%s')",
              got3 ? line3 : "(none)");
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "menu closed after search launch");

    /* 4c. Escape clears the search first, closes the menu second */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    PANEL_WAIT(t, seat && seat->grab_surface == the_menu(t)->surface);
    xw_compositor_inject_key(t->comp, 34 /* KEY_G */, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, 34, false);
    pump_ms(t, 40);
    xw_compositor_inject_key(t->comp, K_ESC, true); /* clear search */
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_ESC, false);
    pump_ms(t, 100);
    XWT_CHECK(n_top_popups(t) == 1, "first Escape cleared the search only");
    xw_compositor_inject_key(t->comp, K_ESC, true); /* close */
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_ESC, false);
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "second Escape closed the menu");

    /* 5. outside click dismisses (press on the desktop background) */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    xw_compositor_inject_pointer_motion(t->comp, 800, 400);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "outside click dismissed the menu");
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after outside dismissal");

    /* 6. exit button with the menu previously opened: the async ctl
     * round trip must not freeze or kill the panel */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);

    /* the exit button is found by its red fill (same scan as the
     * exit-button test) */
    int red_x = -1;
    for (int i = 0; i < 1000 && red_x < 0; i++) {
        xwt_pump(t);
        for (int x = 1000; x < 1280; x++) {
            if (pixel_at(t, x, 14) == 0xffa33434) {
                red_x = x;
                break;
            }
        }
        if (red_x < 0)
            usleep(10000);
    }
    XWT_ASSERT(red_x > 0);
    xw_compositor_inject_pointer_motion(t->comp, red_x + 10, 14);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    /* service the async exit-dialog round trip (a second ctl line) */
    char line2[128] = {0};
    bool got2 = false;
    for (int i = 0; i < 400 && !got2; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line2, sizeof(line2), &got2);
        usleep(10000);
    }
    XWT_CHECK(got2 && strncmp(line2, "exit-dialog", 11) == 0,
              "exit button fired the ctl line (got '%s')",
              got2 ? line2 : "(none)");
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after exit click + menu open");

    reap(&pid);
    unlink(ctl_path);
    close(lfd);
}


/* the v2 menu: categories, favorites, and scrolling. A fixture with
 * categorized entries + a favorites config drives the two-pane flow:
 * click a category row -> the app pane re-lists; favorites resolve
 * from the config; the wheel scrolls long lists. */
static void fake_appdir2(char *out, size_t outn) {
    snprintf(out, outn, "%s/apps2-%d", g_runtimedir(), (int)getpid());
    mkdir(out, 0755);
    char appsub[340];
    snprintf(appsub, sizeof(appsub), "%.320s/applications", out);
    mkdir(appsub, 0755);
    static const char *const entries[] = {
        "[Desktop Entry]\nType=Application\nName=Web Browser\n"
        "Exec=/bin/browser\nCategories=Network;WebBrowser;\n"
        "Icon=web\n",
        "[Desktop Entry]\nType=Application\nName=Text Editor\n"
        "Exec=/bin/editor\nCategories=Utility;TextEditor;\n",
        "[Desktop Entry]\nType=Application\nName=File Manager\n"
        "Exec=/bin/fm\nCategories=System;FileManager;\n",
        "[Desktop Entry]\nType=Application\nName=Sound Mixer\n"
        "Exec=/bin/mixer\nCategories=AudioVideo;\n",
    };
    static const char *const files[] = {"web-browser.desktop", "text-editor.desktop",
                                        "file-manager.desktop", "sound-mixer.desktop"};
    for (size_t i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", appsub, files[i]);
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(entries[i], f);
            fclose(f);
        }
    }
    /* 40 filler entries in Accessories: forces the app pane to
     * overflow and scroll */
    for (int i = 0; i < 40; i++) {
        char path[512], name[64];
        snprintf(name, sizeof(name), "Filler %02d", i);
        snprintf(path, sizeof(path), "%s/filler-%02d.desktop", appsub, i);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\n"
                       "Exec=/bin/filler%d\nCategories=Utility;\n",
                    name, i);
            fclose(f);
        }
    }
}

static void test_panel_menu_v2(struct xwt_ctx *t) {
    char appdir[300];
    fake_appdir2(appdir, sizeof(appdir));
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/empty2-%d", g_runtimedir(),
             (int)getpid());
    mkdir(empty, 0755);
    setenv("XDG_DATA_HOME", appdir, 1);
    setenv("XDG_DATA_DIRS", empty, 1);

    /* favorites: the browser */
    char conf[256];
    snprintf(conf, sizeof(conf), "%s/panel-fav.conf", g_runtimedir());
    FILE *cf = fopen(conf, "w");
    XWT_ASSERT(cf);
    fputs("[panel]\nfavorites=web-browser\n", cf);
    fclose(cf);

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
    XWT_ASSERT(listen(lfd, 8) == 0);

    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        setenv("XW_PANEL_CONF", conf, 1);
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        setenv("XW_PANEL_TRACE", "1", 1);
        int logfd = open("/tmp/xw-panel-child-menu2.log",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(panel_path(), "xw-panel", NULL);
        _exit(127);
    }
    XWT_ASSERT(pid > 0);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);
    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);

    const int ROW = XWC_LINE_H + 10;
    const int PANE_X = 3 + 2;            /* popup x + pad */
    const int APP_X = 3 + 2 + 176;       /* app pane left edge */

    /* open the menu; default selection = Favorites (first category) */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, the_menu(t) && the_menu(t)->mapped);
    PANEL_WAIT(t, seat->grab_surface == the_menu(t)->surface);
    struct xw_popup *pu = the_menu(t);
    XWT_ASSERT(pu);
    /* rows: favorites(1) + all + accessories + internet + multimedia +
     * system + 2 pad rows = cats+2; 44 apps overflow the pane */
    XWT_CHECK(pu->h > 8 * ROW, "menu tall enough to scroll (%d)", pu->h);

    /* 1. Favorites is the default pane: one app (the browser) */
    xw_compositor_inject_pointer_motion(t->comp, APP_X + 60,
                                        30 + 2 + ROW + ROW / 2);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    char line[128] = {0};
    bool got = false;
    for (int i = 0; i < 300 && !got; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line, sizeof(line), &got);
        usleep(10000);
    }
    XWT_CHECK(got && strcmp(line, "run /bin/browser") == 0,
              "favorites launch the pinned app (got '%s')",
              got ? line : "(none)");
    PANEL_WAIT(t, n_top_popups(t) == 0);

    /* 2. categories: click the Internet row, the pane re-lists */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    PANEL_WAIT(t, seat->grab_surface == the_menu(t)->surface);
    /* category rows start below the search header; display order:
     * Favorites 0, All 1, Accessories 2, Multimedia 3, Internet 4 */
    int cat_row = 4;
    xw_compositor_inject_pointer_motion(
        t->comp, PANE_X + 40, 30 + 2 + ROW + cat_row * ROW + ROW / 2);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    pump_ms(t, 100);
    XWT_CHECK(n_top_popups(t) == 1, "category click keeps the menu open");
    /* the Internet pane lists exactly one app: the browser */
    xw_compositor_inject_pointer_motion(t->comp, APP_X + 60,
                                        30 + 2 + ROW + ROW / 2);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    char line2[128] = {0};
    bool got2 = false;
    for (int i = 0; i < 300 && !got2; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line2, sizeof(line2), &got2);
        usleep(10000);
    }
    XWT_CHECK(got2 && strcmp(line2, "run /bin/browser") == 0,
              "Internet category lists the browser (got '%s')",
              got2 ? line2 : "(none)");
    PANEL_WAIT(t, n_top_popups(t) == 0);

    /* 3. scrolling: All has 44 apps; the wheel scrolls the pane and
     * the click hits a later entry than the alphabetical first */
    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    PANEL_WAIT(t, seat->grab_surface == the_menu(t)->surface);
    /* select "All" (row 1) */
    xw_compositor_inject_pointer_motion(
        t->comp, PANE_X + 40, 30 + 2 + ROW + 1 * ROW + ROW / 2);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    pump_ms(t, 100);
    /* wheel down over the app pane: several notches */
    for (int i = 0; i < 6; i++) {
        xw_compositor_inject_pointer_axis(t->comp, 0, 10.0);
        pump_ms(t, 60);
    }
    /* the row-0 click now launches a filler (scrolled), not the
     * browser (alphabetically first among the fixture apps) */
    xw_compositor_inject_pointer_motion(t->comp, APP_X + 60,
                                        30 + 2 + ROW + ROW / 2);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    char line3[128] = {0};
    bool got3 = false;
    for (int i = 0; i < 300 && !got3; i++) {
        xwt_pump(t);
        handled_ctl_line(lfd, line3, sizeof(line3), &got3);
        usleep(10000);
    }
    XWT_CHECK(got3 && strncmp(line3, "run /bin/filler", 15) == 0,
              "scrolled pane launches a later entry (got '%s')",
              got3 ? line3 : "(none)");
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after the v2 menu flow");

    reap(&pid);
    unlink(ctl_path);
    close(lfd);
    unlink(conf);
}


/* the graphical pager: workspace boxes carry window miniatures. A
 * window moved to workspace 2 lights a tile in box 2's interior (the
 * tile color, not the box chrome); the active workspace box is the
 * highlighted one; clicking a box still switches workspaces. */
static void test_panel_pager(struct xwt_ctx *t) {
    pid_t pid = spawn_panel(t, "-pager");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* two windows; both land on the current workspace (0) */
    struct xwc_win *a = xwt_window_solid(t, 0xff112233, 200, 100, "PagerA");
    struct xwc_win *b = xwt_window_solid(t, 0xff445566, 200, 100, "PagerB");
    XWT_ASSERT(a && b);
    XWT_WAIT(t, t->comp->wm->focused && t->comp->wm->focused->ws == 0);

    /* find the ws boxes by the run scan (right region, before clock) */
    struct bar_run runs[MAX_RUNS];
    int n = 0;
    int exit_i = -1;
    for (int i = 0; i < 400 && exit_i < 0; i++) {
        xwt_pump(t);
        n = bar_scan_runs(t, runs);
        for (int k = n - 1; k >= 0; k--)
            if (runs[k].color == 0xffa33434 || runs[k].color == 0xffc94b4b) {
                exit_i = k;
                break;
            }
    }
    /* the ws boxes are the runs immediately left of the clock; task
     * buttons may sit between the start button and them */
    int clock_i = exit_i - 1;
    int n_ws = t->comp->wm->ws_count;
    int first_ws = clock_i - n_ws;
    XWT_ASSERT(first_ws >= 1);
    int box1_x0 = runs[first_ws].x0, box1_x1 = runs[first_ws].x1;
    int box2_x0 = runs[first_ws + 1].x0, box2_x1 = runs[first_ws + 1].x1;

    /* 1. tiles render in box 1 (both windows live on workspace 0) */
    bool tile = false;
    for (int i = 0; i < 400 && !tile; i++) {
        xwt_pump(t);
        for (int x = box1_x0 + 6; x < box1_x1 - 6 && !tile; x++)
            for (int yy = 4; yy < first_top_layer(t)->h - 4; yy++)
                if (pixel_at(t, x, yy) == 0xff55627a ||
                    pixel_at(t, x, yy) == 0xff3584e4) {
                    tile = true;
                    break;
                }
        if (!tile)
            usleep(10000);
    }
    XWT_CHECK(tile, "window tiles rendered inside workspace box 1");

    /* 2. move window A to workspace 2 (1): its tile appears in box 2 */
    struct xw_window *wa = t->comp->wm->focused;
    XWT_ASSERT(wa);
    xw_wm_window_to_workspace(t->comp->wm, wa, 1);
    bool tile2 = false;
    for (int i = 0; i < 400 && !tile2; i++) {
        xwt_pump(t);
        for (int x = box2_x0 + 6; x < box2_x1 - 6 && !tile2; x++)
            for (int yy = 4; yy < first_top_layer(t)->h - 4; yy++)
                if (pixel_at(t, x, yy) == 0xff55627a ||
                    pixel_at(t, x, yy) == 0xff3584e4) {
                    tile2 = true;
                    break;
                }
        if (!tile2)
            usleep(10000);
    }
    XWT_CHECK(tile2, "moved window lights a tile in box 2");
    /* and box 1 still shows the other window */
    tile = false;
    for (int x = box1_x0 + 6; x < box1_x1 - 6 && !tile; x++)
        for (int yy = 4; yy < first_top_layer(t)->h - 4; yy++)
            if (pixel_at(t, x, yy) == 0xff55627a ||
                pixel_at(t, x, yy) == 0xff3584e4)
                tile = true;
    XWT_CHECK(tile, "box 1 keeps its remaining window tile");

    /* 3. switching to workspace 2 re-lights the active box: the box 2
     * border region gains the active border color */
    xw_compositor_inject_pointer_motion(t->comp, (box2_x0 + box2_x1) / 2, 15);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, t->comp->wm->ws_current == 1);
    XWT_CHECK(t->comp->wm->ws_current == 1,
              "clicking a pager box switches workspaces");
    bool active_border = false;
    for (int i = 0; i < 300 && !active_border; i++) {
        xwt_pump(t);
        for (int x = box2_x0; x < box2_x1 && !active_border; x++) {
            if (pixel_at(t, x, 3) == 0xff88b0ef ||
                pixel_at(t, x, first_top_layer(t)->h - 4) == 0xff88b0ef)
                active_border = true;
        }
        if (!active_border)
            usleep(10000);
    }
    XWT_CHECK(active_border, "the active workspace box is outlined");

    /* 4. sticky windows tile every box (dimmed) */
    struct xw_window *wb = NULL;
    wl_list_for_each(wb, &t->comp->wm->windows, link) {
        if (strcmp(wb->title, "PagerB") == 0)
            break;
    }
    XWT_ASSERT(wb);
    wb->ws = -1;
    xw_workspace_info_notify(t->comp, wb);
    bool sticky_tile = false;
    for (int i = 0; i < 400 && !sticky_tile; i++) {
        xwt_pump(t);
        /* box 3 has no windows of its own: a sticky tile appears */
        for (int x = runs[first_ws + 2].x0 + 6;
             x < runs[first_ws + 2].x1 - 6 && !sticky_tile; x++)
            for (int yy = 4; yy < first_top_layer(t)->h - 4; yy++)
                if (pixel_at(t, x, yy) == 0xff454f60) {
                    sticky_tile = true;
                    break;
                }
        if (!sticky_tile)
            usleep(10000);
    }
    XWT_CHECK(sticky_tile, "sticky window tiles every box (dimmed)");

    XWT_CHECK(kill(pid, 0) == 0, "panel alive after the pager flow");
    xwc_win_destroy(a);
    xwc_win_destroy(b);
    reap(&pid);
}

/* compositor shutdown while the menu is open: the panel must exit
 * cleanly (no crash, exit status 0) */
static void test_panel_menu_compositor_shutdown(struct xwt_ctx *t) {
    char appdir[300];
    fake_appdir(appdir, sizeof(appdir));
    char empty[300];
    snprintf(empty, sizeof(empty), "%s/emptydata-%d", g_runtimedir(),
             (int)getpid());
    setenv("XDG_DATA_HOME", appdir, 1);
    setenv("XDG_DATA_DIRS", empty, 1);

    pid_t pid = spawn_panel(t, "-menu-shutdown");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    xw_compositor_inject_pointer_motion(t->comp, START_CX, 15);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    XWT_CHECK(n_top_popups(t) == 1, "menu open when the compositor dies");

    /* SIGTERM the panel with the menu open: clean exit (the menu
     * teardown runs before the connection teardown) */
    kill(pid, SIGTERM);
    int status = 0;
    bool exited = false;
    for (int i = 0; i < 200; i++) {
        xwt_pump(t);
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        usleep(10000);
    }
    XWT_CHECK(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "panel exited cleanly with the menu open (status 0x%x)",
              status);
    /* the compositor reaped the popup surface: no leak into the list */
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "popup gone after the panel exited");
}


/* v0 documented behavior: the clock is display-only — a click neither
 * crashes the panel nor fires any session action */
static void test_panel_clock_click(struct xwt_ctx *t) {
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

    pid_t pid = spawn_panel(t, "-clock");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* locate the clock: the widget run left of the exit button */
    struct bar_run runs[MAX_RUNS];
    int exit_i = -1;
    for (int i = 0; i < 400 && exit_i < 0; i++) {
        xwt_pump(t);
        int n = bar_scan_runs(t, runs);
        for (int k = n - 1; k >= 0; k--)
            if (runs[k].color == 0xffa33434 || runs[k].color == 0xffc94b4b) {
                exit_i = k;
                break;
            }
        if (exit_i < 0)
            usleep(10000);
    }
    XWT_ASSERT(exit_i >= 2);
    int clock_x = (runs[exit_i - 1].x0 + runs[exit_i - 1].x1) / 2;
    int ws_before = t->comp->wm->ws_current;

    /* 1. clicking the clock opens the calendar popup */
    xw_compositor_inject_pointer_motion(t->comp, clock_x, 15);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, the_menu(t) && the_menu(t)->mapped);
    struct xw_popup *cal = the_menu(t);
    XWT_CHECK(cal && cal->mapped, "calendar popup mapped");
    XWT_CHECK(cal && cal->parent == first_top_layer(t)->surface,
              "calendar parented to the bar layer");
    XWT_CHECK(cal && cal->anchor_x >= 900 && cal->anchor_y == 30,
              "calendar anchored under the clock button (%d,%d)",
              cal ? cal->anchor_x : -1, cal ? cal->anchor_y : -1);

    /* 2. today is highlighted: the accent fill exists in the grid */
    bool today_lit = false;
    for (int i = 0; i < 300 && !today_lit; i++) {
        xwt_pump(t);
        for (int y = 34; y < cal->anchor_y + cal->h && !today_lit; y += 2)
            for (int x = cal->anchor_x; x < cal->anchor_x + cal->w; x += 2)
                if (pixel_at(t, x, y) == 0xff3584e4) {
                    today_lit = true;
                    break;
                }
        if (!today_lit)
            usleep(10000);
    }
    XWT_CHECK(today_lit, "today's cell highlighted");

    /* 3. the prev-month arrow changes the header (month text moves) */
    uint64_t hash_before = 0;
    for (int x = cal->anchor_x + 40; x < cal->anchor_x + cal->w - 40; x++)
        hash_before += pixel_at(t, x, cal->anchor_y + 16) +
                       pixel_at(t, x, cal->anchor_y + 18);
    xw_compositor_inject_pointer_motion(t->comp, cal->anchor_x + 16,
                                        cal->anchor_y + 17);
    pump_ms(t, 80);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    pump_ms(t, 120);
    uint64_t hash_after = 0;
    for (int x = cal->anchor_x + 40; x < cal->anchor_x + cal->w - 40; x++)
        hash_after += pixel_at(t, x, cal->anchor_y + 16) +
                       pixel_at(t, x, cal->anchor_y + 18);
    XWT_CHECK(hash_before != hash_after,
              "prev-month navigation changed the header");

    /* 4. Escape closes the calendar */
    xw_compositor_inject_key(t->comp, K_ESC, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_ESC, false);
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "Escape closed the calendar");

    /* 5. reopen + wheel scrolls months; outside click closes */
    xw_compositor_inject_pointer_motion(t->comp, clock_x, 15);
    pump_ms(t, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 1);
    cal = the_menu(t);
    XWT_ASSERT(cal);
    hash_before = 0;
    for (int x = cal->anchor_x + 40; x < cal->anchor_x + cal->w - 40; x++)
        hash_before += pixel_at(t, x, cal->anchor_y + 16) +
                       pixel_at(t, x, cal->anchor_y + 18);
    xw_compositor_inject_pointer_axis(t->comp, 0, 8.0);
    pump_ms(t, 120);
    hash_after = 0;
    for (int x = cal->anchor_x + 40; x < cal->anchor_x + cal->w - 40; x++)
        hash_after += pixel_at(t, x, cal->anchor_y + 16) +
                       pixel_at(t, x, cal->anchor_y + 18);
    XWT_CHECK(hash_before != hash_after, "wheel scrolls the month");
    xw_compositor_inject_pointer_motion(t->comp, 800, 400);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, n_top_popups(t) == 0);
    XWT_CHECK(n_top_popups(t) == 0, "outside click dismissed the calendar");

    /* the clock click never fired session actions or moved workspaces */
    XWT_CHECK(t->comp->wm->ws_current == ws_before,
              "clock clicks changed no workspace");
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    XWT_CHECK(poll(&pfd, 1, 200) == 0, "clock clicks fired no session action");
    XWT_CHECK(kill(pid, 0) == 0, "panel alive after the calendar flow");

    reap(&pid);
    unlink(ctl_path);
    close(lfd);
}

/* layer surface created while the compositor has NO outputs: the
 * request used to be silently dropped (unbound object id) and the
 * client disconnected on its next request. It must be held, then
 * adopted + configured when an output appears. */
static void test_layer_before_outputs(struct xwt_ctx *t) {
    /* the connect roundtrip can return while the client's bind
     * requests are still queued server-side (the sync reply overtakes
     * nothing — the pump dispatches the server BEFORE the client
     * flushes its binds). Pump until the server has digested them, or
     * destroying the output below rejects the queued wl_output bind
     * with a protocol error. */
    pump_ms(t, 150);

    /* remove the headless output: the backend keeps no references, the
     * render loop iterates the (now empty) list */
    struct xw_output *o;
    XWT_ASSERT(!wl_list_empty(&t->comp->outputs));
    o = wl_container_of(t->comp->outputs.next, o, link);
    xw_output_destroy(o);
    XWT_CHECK(wl_list_empty(&t->comp->outputs), "output removed");

    pid_t pid = spawn_panel(t, "-nooutputs");
    XWT_ASSERT(pid > 0);
    pump_ms(t, 400);
    /* the panel must SURVIVE creating a layer surface with no outputs
     * (protocol kill = instant exit under the old code) */
    XWT_CHECK(kill(pid, 0) == 0,
              "panel survives layer creation without outputs");

    /* an output appears: the held layer surface is adopted, configured,
     * and the panel maps + renders */
    struct xw_output *o2 = xw_output_create(t->comp, "TEST", 0, 0, 1280, 720, 1);
    XWT_ASSERT(o2);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);
    XWT_CHECK(first_top_layer(t) && first_top_layer(t)->mapped,
              "layer surface adopted + mapped when the output appeared");
    PANEL_WAIT(t, bar_rendered(t));
    XWT_CHECK(bar_rendered(t), "bar rendered after late output");
    XWT_CHECK(first_top_layer(t)->output == o2,
              "adopted surface is bound to the new output");

    /* and it is interactive end-to-end */
    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    int ws2b_x = -1;
    for (int i = 0; i < 300 && ws2b_x < 0; i++) {
        xwt_pump(t);
        ws2b_x = ws_box_cx(t, 1);
        if (ws2b_x < 0)
            usleep(10000);
    }
    XWT_ASSERT(ws2b_x > 0);
    xw_compositor_inject_pointer_motion(t->comp, ws2b_x, 14);
    XWT_WAIT(t, seat->ptr_focus == first_top_layer(t)->surface);
    XWT_CHECK(seat->ptr_focus == first_top_layer(t)->surface,
              "late-adopted panel takes pointer focus");
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    PANEL_WAIT(t, t->comp->wm->ws_current == 1);
    XWT_CHECK(t->comp->wm->ws_current == 1,
              "click on late-adopted panel switches workspace");

    reap(&pid);
}

/* the compositor as a compositor: no panel process, no session manager
 * — windows still map, take pointer focus, and receive clicks */
static int g_nopanel_button_count;
static struct xwc_win *g_nopanel_win;
static void nopanel_button(struct xwc_win *w, uint32_t button, bool down,
                           int x, int y, void *ud) {
    (void)button;
    (void)x;
    (void)y;
    (void)ud;
    if (down && w == g_nopanel_win)
        g_nopanel_button_count++;
}

/* a mapped, clickable window without the panel: configure draws the
 * solid fill and commits (an uncommitted window never maps and never
 * takes focus — xwt_window_solid's pattern plus a button callback) */
static void nopanel_configure(struct xwc_win *w, int width, int height,
                              void *ud) {
    (void)width;
    (void)height;
    uint32_t color = *(uint32_t *)ud;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, color);
    xwc_win_commit(w);
}

static void test_compositor_without_panel(struct xwt_ctx *t) {
    XWT_CHECK(wl_list_empty(&t->comp->wm->layers[2]),
              "no layer-shell surfaces without a panel");
    XWT_CHECK(wl_list_empty(&t->comp->wm->layers[3]), "overlay empty");

    g_nopanel_button_count = 0;
    g_nopanel_win = NULL;
    static uint32_t color = 0xff667788;
    struct xwc_callbacks cb = {
        .button = nopanel_button,
        .motion = NULL,
        .configure = nopanel_configure,
        .close = NULL,
        .key = NULL,
        .ud = &color,
    };
    g_nopanel_win = xwc_win_create(&t->client, &cb, "Solo", "solo", 400, 300);
    XWT_ASSERT(g_nopanel_win);
    xwt_pump(t);
    xwt_pump(t);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    XWT_WAIT(t, t->comp->wm->focused && t->comp->wm->focused->title &&
             strcmp(t->comp->wm->focused->title, "Solo") == 0);
    XWT_ASSERT(t->comp->wm->focused); /* do not deref a failed wait */

    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    int wx = t->comp->wm->focused->x, wy = t->comp->wm->focused->y;
    xw_compositor_inject_pointer_motion(t->comp, wx + 100, wy + 100);
    XWT_WAIT(t, seat->ptr_focus == t->comp->wm->focused->surface);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    XWT_WAIT(t, g_nopanel_button_count == 1);
    XWT_CHECK(g_nopanel_button_count == 1,
              "window received the click without any panel existing");

    xwc_win_destroy(g_nopanel_win);
    g_nopanel_win = NULL;
}

static const struct xwt_test tests[] = {
    {"tasklist-client", test_tasklist_client},
    {"tasklist-workspace", test_tasklist_workspace},
    {"workspace-client", test_workspaces_client},
    {"panel-maps", test_panel_maps},
    {"panel-clicks", test_panel_clicks},
    {"panel-exit-button", test_panel_exit_button},
    {"panel-pointer-focus", test_panel_pointer_focus},
    {"late-pointer-enter-replay", test_late_pointer_enter_replay},
    {"panel-launcher", test_panel_launcher},
    {"panel-start-repeated", test_panel_start_repeated},
    {"panel-menu", test_panel_menu},
    {"panel-menu-v2", test_panel_menu_v2},
    {"panel-pager", test_panel_pager},
    {"panel-menu-compositor-shutdown", test_panel_menu_compositor_shutdown},
    {"panel-clock-click", test_panel_clock_click},
    {"layer-before-outputs", test_layer_before_outputs},
    {"compositor-without-panel", test_compositor_without_panel},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
