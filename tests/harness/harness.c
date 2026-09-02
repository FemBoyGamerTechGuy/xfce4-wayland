/* harness.c — test harness implementation. */
#include "xwtest.h"

#include <sys/stat.h>
#include <unistd.h>

int xwt_failures = 0;
int xwt_tests = 0;
const char *xwt_current = "?";

static char s_runtimedir[128];

const char *g_runtimedir(void) { return s_runtimedir; }

#define XWT_MAX_TESTS 96
static struct xwt_test g_tests[XWT_MAX_TESTS];
static int g_n_tests;

void xwt_register(const struct xwt_test *tests, int n) {
    for (int i = 0; i < n && g_n_tests < XWT_MAX_TESTS; i++)
        g_tests[g_n_tests++] = tests[i];
}

static void xwt_pump_server(void *ud) {
    /* minimal pump used while the client syncs: run the server and read
     * whatever the client sent */
    struct xwt_ctx *t = ud;
    xw_compositor_dispatch(t->comp, 0);
    wl_display_flush(t->client.display);
    if (t->client_dead)
        return; /* connection error earlier: stop touching the display —
                   wl_display_prepare_read spins forever on a dead
                   connection and used to wedge the whole suite */
    while (wl_display_prepare_read(t->client.display) != 0) {
        if (wl_display_dispatch_pending(t->client.display) < 0) {
            t->client_dead = true;
            return;
        }
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(t->client.display),
                         .events = POLLIN};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN) {
        if (wl_display_read_events(t->client.display) < 0) {
            t->client_dead = true;
            return;
        }
    } else {
        wl_display_cancel_read(t->client.display);
    }
    if (wl_display_dispatch_pending(t->client.display) < 0)
        t->client_dead = true;
    xw_compositor_dispatch(t->comp, 0);
}

int xwt_begin(struct xwt_ctx *t, const char *config_dir) {
    static int instance = 0;
    memset(t, 0, sizeof(*t));
    snprintf(t->socket_name, sizeof(t->socket_name), "xwt-%d-%d", getpid(),
             instance++);

    if (!s_runtimedir[0]) {
        snprintf(s_runtimedir, sizeof(s_runtimedir), "/tmp/xwt-%d", getpid());
        mkdir(s_runtimedir, 0700);
    }
    setenv("XDG_RUNTIME_DIR", s_runtimedir, 1);

    struct xw_compositor_config cfg = {0};
    cfg.config_dir = config_dir;
    cfg.socket_name = t->socket_name;
    cfg.log_level = XW_LOG_ERROR;

    t->comp = xw_compositor_create(&cfg);
    if (!t->comp) {
        printf("  FATAL: compositor create failed\n");
        return -1;
    }
    if (xwc_connect_pumped(&t->client, t->socket_name, xwt_pump_server, t) < 0) {
        printf("  FATAL: client connect failed\n");
        xw_compositor_destroy(t->comp);
        t->comp = NULL;
        return -1;
    }
    return 0;
}

void xwt_end(struct xwt_ctx *t) {
    if (t->client.display)
        xwc_disconnect(&t->client);
    if (t->comp) {
        xw_compositor_destroy(t->comp);
        t->comp = NULL;
    }
}

void xwt_pump(struct xwt_ctx *t) { xwt_pump_server(t); }

int xwt_run_all(void) {
    const char *filter = getenv("XWT_FILTER");
    int selected = 0;
    for (int i = 0; i < g_n_tests; i++)
        if (!filter || !*filter || strstr(g_tests[i].name, filter))
            selected++;
    printf("xw test suite: %d tests%s%s%s\n", selected,
           filter && *filter ? " (filter: \"" : "",
           filter && *filter ? filter : "",
           filter && *filter ? "\")" : "");
    struct xwt_ctx t;
    int passed = 0;
    for (int i = 0; i < g_n_tests; i++) {
        if (filter && *filter && !strstr(g_tests[i].name, filter))
            continue;
        xwt_current = g_tests[i].name;
        xwt_tests++;
        printf("[%2d] %-40s", i + 1, g_tests[i].name);
        fflush(stdout);
        int before = xwt_failures;
        if (xwt_begin(&t, NULL) < 0) {
            xwt_failures++;
            printf("FAILED (setup)\n");
            continue;
        }
        g_tests[i].fn(&t);
        xwt_end(&t);
        if (xwt_failures == before) {
            passed++;
            printf("ok\n");
        } else {
            printf("(failure)\n");
        }
    }
    printf("%d/%d tests passed, %d failures\n", passed, xwt_tests,
           xwt_failures);
    return xwt_failures ? 1 : 0;
}
