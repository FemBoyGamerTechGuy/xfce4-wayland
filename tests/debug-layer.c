/* debug-layer: reproduce the suite's exit-dialog child against an
 * in-process compositor with tracing */
#include "xwtest.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    /* reproduce the suite's history: N compositor instances first (default 9
     * to match the full-suite run; set XWT_PREINSTANCES=0 to mimic an
     * isolated session-test run) */
    const char *pre = getenv("XWT_PREINSTANCES");
    int n_pre = pre ? atoi(pre) : 9;
    for (int i = 0; i < n_pre; i++) {
        struct xwt_ctx prev;
        if (xwt_begin(&prev, NULL) < 0)
            return 1;
        xwt_end(&prev);
    }
    struct xwt_ctx t;
    if (xwt_begin(&t, NULL) < 0)
        return 1;
    printf("compositor socket: %s (rtd %s)\n", t.socket_name,
           g_runtimedir());

    pid_t pid = fork();
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t.socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        int logfd = open("/tmp/xw-exit-child.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        const char *bin = getenv("XW_EXIT_BIN");
        if (!bin)
            bin = "build/bin/xw-exit";
        execl(bin, "xw-exit", NULL);
        fprintf(stderr, "child: exec %s failed: %s\n", bin, strerror(errno));
        _exit(127);
    }
    /* pump and watch layer state */
    for (int i = 0; i < 200; i++) {
        xwt_pump(&t);
        int n = 0;
        for (int l = 0; l < 4; l++) {
            struct xw_layer_surface *ls;
            wl_list_for_each(ls, &t.comp->wm->layers[l], link) n++;
        }
        if (n && i % 20 == 0) {
            struct xw_seat *seat = xw_seat_first(t.comp);
            struct xw_layer_surface *ls = NULL;
            for (int l = 3; l >= 0 && !ls; l--)
                wl_list_for_each(ls, &t.comp->wm->layers[l], link) break;
            struct xw_output *o = wl_container_of(t.comp->outputs.next, o, link);
            const uint32_t *pix =
                (const uint32_t *)pixman_image_get_data(o->native);
            int pw = pixman_image_get_width(o->native);
            fprintf(stderr,
                    "pump %d: layers=%d mapped=%d kb=%u focus=%p layer=%p "
                    "ls_xywh=%d,%d %dx%d dmg=%d center=0x%08x\n",
                    i, n, ls ? ls->mapped : 0, ls ? ls->keyboard_interactivity : 0,
                    (void*)(seat ? seat->kb_focus : NULL), (void*)(ls ? ls->surface : NULL),
                    ls ? ls->x : -1, ls ? ls->y : -1, ls ? ls->w : -1, ls ? ls->h : -1,
                    pixman_region_not_empty(&o->damage),
                    pix[pw * (o->height / 2) + o->width / 2]);
        }
        if (i == 50) {
            fprintf(stderr, "injecting Escape\n");
            xw_compositor_inject_key(t.comp, K_ESC, true);
            xwt_pump(&t);
            xw_compositor_inject_key(t.comp, K_ESC, false);
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            printf("child exited rc=%d after %d pumps\n",
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1, i);
            break;
        }
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    xwt_end(&t);
    return 0;
}
