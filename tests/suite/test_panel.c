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

    setenv("XW_TERMINAL", "/bin/true", 1); /* inherited by the panel */
    pid_t pid = spawn_panel(t, "-launcher");
    unsetenv("XW_TERMINAL");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, first_top_layer(t) && first_top_layer(t)->mapped);

    /* launcher occupies x=3..39 at bar height 28 */
    xw_compositor_inject_pointer_motion(t->comp, 20, 14);
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

    /* locate the exit button (red fill) — the clock is its left
     * neighbor; click inside the clock, clear of the gap */
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
    XWT_ASSERT(red_x > 0);
    int clock_x = red_x - 30;
    int ws_before = t->comp->wm->ws_current;

    xw_compositor_inject_pointer_motion(t->comp, clock_x, 14);
    pump_ms(t, 60);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    pump_ms(t, 200);

    XWT_CHECK(kill(pid, 0) == 0, "panel alive after clock click (v0)");
    XWT_CHECK(t->comp->wm->ws_current == ws_before,
              "clock click changed nothing (display-only v0)");
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    XWT_CHECK(poll(&pfd, 1, 200) == 0,
              "clock click fired no session action");

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
    xw_compositor_inject_pointer_motion(t->comp, 42 + 27 + 12, 14);
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
    {"workspace-client", test_workspaces_client},
    {"panel-maps", test_panel_maps},
    {"panel-clicks", test_panel_clicks},
    {"panel-exit-button", test_panel_exit_button},
    {"panel-pointer-focus", test_panel_pointer_focus},
    {"late-pointer-enter-replay", test_late_pointer_enter_replay},
    {"panel-launcher", test_panel_launcher},
    {"panel-clock-click", test_panel_clock_click},
    {"layer-before-outputs", test_layer_before_outputs},
    {"compositor-without-panel", test_compositor_without_panel},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
