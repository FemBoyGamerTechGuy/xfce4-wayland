/* debug-layer: reproduce the suite's exit-dialog child against an
 * in-process compositor with tracing */
#include "xwtest.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    /* reproduce the suite's history: 9 compositor instances first */
    for (int i = 0; i < 9; i++) {
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
        fprintf(stderr, "child: exec xw-exit\n");
        execl("/tmp/xw-exit-dbg", "xw-exit", NULL);
        fprintf(stderr, "child: exec failed: %s\n", strerror(errno));
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
            fprintf(stderr,
                    "pump %d: layers=%d mapped=%d kb=%u focus=%p layer=%p\n",
                    i, n, ls ? ls->mapped : 0, ls ? ls->keyboard_interactivity : 0,
                    (void*)(seat ? seat->kb_focus : NULL), (void*)(ls ? ls->surface : NULL));
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
