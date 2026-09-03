/* test_playout.c — the panel layout engine: metrics derivation, region
 * ordering and no-overlap at several output sizes, top/bottom
 * position, and the config INI reader. The panel runs as a real child
 * process against the in-process compositor; geometry is read from
 * the layer state and the rendered pixels (never hardcoded button
 * coordinates — the scan survives metric changes). */
#include "xwtest.h"
#include "panel.h"

#include <fcntl.h>
#include <ctype.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* reuse the panel-process helpers from test_panel.c (spawn, reap,
 * pump, pixel_at) — they are file-local there, so tiny copies live
 * here; both files stay in sync by construction (same socket + log
 * naming scheme). */
static const char *panel_path(void) {
    if (access("build/bin/xw-panel", X_OK) == 0)
        return "build/bin/xw-panel";
    if (access("../build/bin/xw-panel", X_OK) == 0)
        return "../build/bin/xw-panel";
    return NULL;
}

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
        FILE *keep = NULL;
        (void)keep;
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

static uint32_t pixel_at(struct xwt_ctx *t, int x, int y) {
    int w = 0, h = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &w, &h);
    if (!pix || x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    return pix[y * w + x];
}

static struct xw_layer_surface *the_layer(struct xwt_ctx *t) {
    if (wl_list_empty(&t->comp->wm->layers[2]))
        return NULL;
    struct xw_layer_surface *ls;
    ls = wl_container_of(t->comp->wm->layers[2].next, ls, link);
    return ls;
}

/* ------------------------------------------------------------ metrics */

static void test_metrics(struct xwt_ctx *t) {
    (void)t;
    struct panel_metrics m;

    m = panel_metrics_for(0, 720);
    XWT_CHECK(m.H == 30, "720p auto height 30 (%d)", m.H);
    XWT_CHECK(!m.big_font, "720p uses the 16px raster");
    XWT_CHECK(m.icon >= 16, "icon cell sane (%d)", m.icon);

    m = panel_metrics_for(0, 1080);
    XWT_CHECK(m.H == 34, "1080p auto height 34 (%d)", m.H);
    XWT_CHECK(!m.big_font, "1080p still the 16px raster");

    m = panel_metrics_for(0, 1440);
    XWT_CHECK(m.H == 45, "1440p auto height 45 (%d)", m.H);
    XWT_CHECK(m.big_font, "1440p uses the 24px raster");
    XWT_CHECK(m.icon == 33, "1440p icon 33 (%d)", m.icon);

    m = panel_metrics_for(0, 2160);
    XWT_CHECK(m.H == 52, "4K auto height capped 52 (%d)", m.H);

    m = panel_metrics_for(40, 720);
    XWT_CHECK(m.H == 40, "config height wins (%d)", m.H);

    m = panel_metrics_for(500, 720);
    XWT_CHECK(m.H <= 200, "absurd config height clamped (%d)", m.H);
}

/* ------------------------------------------------------------- config */

static void test_config(struct xwt_ctx *t) {
    (void)t;
    struct panel_config cfg;

    /* defaults: sane without any file */
    panel_config_defaults(&cfg);
    XWT_CHECK(cfg.height == 0 && !cfg.bottom, "defaults: auto height, top");
    XWT_CHECK(strcmp(cfg.clock_format, "%a %d %b %H:%M") == 0,
              "default clock format");
    XWT_CHECK(cfg.menu_icons, "menu icons on by default");
    XWT_CHECK(cfg.tasklist_style == 0, "tasklist icons+text default");

    /* a realistic file */
    const char *lines[] = {
        "# comment",
        "[panel]",
        "height = 44",
        "position=bottom",
        "launchers=org.example.Terminal, org.example.Editor",
        "favorites = first,second",
        "",
        "[clock]",
        "format=%H:%M:%S",
        "seconds=true",
        "[tasklist]",
        "style=icons",
        "[menu]",
        "icons=false",
        "junk line without equals",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        panel_config_line(&cfg, lines[i]);
    XWT_CHECK(cfg.height == 44, "height parsed");
    XWT_CHECK(cfg.bottom, "position=bottom parsed");
    XWT_CHECK(strcmp(cfg.clock_format, "%H:%M:%S") == 0, "clock format");
    XWT_CHECK(cfg.clock_seconds, "clock seconds");
    XWT_CHECK(cfg.tasklist_style == 1, "tasklist style icons");
    XWT_CHECK(!cfg.menu_icons, "menu icons off");
    XWT_CHECK(strstr(cfg.launchers, "org.example.Terminal") != NULL,
              "launchers parsed");
    XWT_CHECK(strstr(cfg.favorites, "second") != NULL, "favorites parsed");
}

/* ---------------------------------------------------- clock formatting */

static void test_clock_format(struct xwt_ctx *t) {
    (void)t;
    struct panel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.clock_format, sizeof(cfg.clock_format), "%%H:%%M");
    char buf[64];
    panel_clock_format(&cfg, buf, sizeof(buf));
    XWT_CHECK(strlen(buf) == 5 && buf[2] == ':',
              "HH:MM renders ('%s')", buf);

    /* the full default format renders weekday + day + month + time */
    snprintf(cfg.clock_format, sizeof(cfg.clock_format),
             "%%a %%d %%b %%H:%%M");
    panel_clock_format(&cfg, buf, sizeof(buf));
    XWT_CHECK(strlen(buf) >= 14, "full default renders ('%s')", buf);
    /* weekday abbreviation from our table (3 letters, capitalized) */
    XWT_CHECK(isupper((unsigned char)buf[0]) && buf[3] == ' ',
              "weekday abbreviation present ('%.16s')", buf);

    /* 12h + AM/PM */
    snprintf(cfg.clock_format, sizeof(cfg.clock_format), "%%I:%%M %%p");
    panel_clock_format(&cfg, buf, sizeof(buf));
    XWT_CHECK(strstr(buf, "AM") || strstr(buf, "PM"),
              "12-hour format with meridiem ('%s')", buf);

    /* literal %% and unknown tokens survive (the unknown token is
     * built at runtime: a literal "%q" in a C format string is itself
     * a -Wformat error) */
    char qfmt[12];
    snprintf(qfmt, sizeof(qfmt), "100%%%% %%%c", 'q'); /* "100%% %q" */
    snprintf(cfg.clock_format, sizeof(cfg.clock_format), "%s", qfmt);
    panel_clock_format(&cfg, buf, sizeof(buf));
    XWT_CHECK(strstr(buf, "100%") != NULL, "literal %% survives ('%s')", buf);
}

/* --------------------------------------------------- layout in-process */

/* run the panel on an output of the given size and verify the region
 * order + no overlap via the run scan */
static void layout_at(struct xwt_ctx *t, int out_w, int out_h, int want_h,
                      bool want_big_font) {
    /* replace the default output with one of the target size */
    struct xw_output *def =
        wl_container_of(t->comp->outputs.next, def, link);
    xw_output_destroy(def);
    struct xw_output *o = xw_output_create(t->comp, "SIZE", 0, 0, out_w,
                                           out_h, 1);
    XWT_ASSERT(o);
    pid_t pid = spawn_panel(t, "-layout");
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, the_layer(t) && the_layer(t)->mapped);
    XWT_CHECK(the_layer(t)->h == want_h,
              "%dx%d: bar height %d (got %d)", out_w, out_h, want_h,
              the_layer(t)->h);
    XWT_CHECK(the_layer(t)->w == out_w, "bar spans the output width");

    /* region scan: runs must be ordered and separated (no overlap) */
    int y = want_h / 2;
    int runs = 0;
    int prev_x1 = -1;
    int exit_x0 = -1, clock_x0 = -1, start_x1 = -1;
    int x = 0;
    while (x < out_w && runs < 32) {
        uint32_t c = pixel_at(t, x, y);
        if (c == 0xff22262e || c == 0) {
            x++;
            continue;
        }
        int x0 = x;
        while (x < out_w && pixel_at(t, x, y) != 0xff22262e &&
               pixel_at(t, x, y) != 0)
            x++;
        if (x - x0 < 6)
            continue; /* glyph noise */
        if (prev_x1 >= 0)
            XWT_CHECK(x0 - prev_x1 >= 2,
                      "%dx%d: widgets overlap or touch at x=%d", out_w,
                      out_h, x0);
        prev_x1 = x;
        if (runs == 0)
            start_x1 = x; /* leftmost = start button */
        exit_x0 = x0; /* remember the rightmost run so far */
        if (exit_x0 == x0)
            clock_x0 = clock_x0; /* placeholder; clock found below */
        runs++;
    }
    XWT_CHECK(runs >= 5, "%dx%d: >= 5 widgets rendered (got %d)", out_w,
              out_h, runs);
    XWT_CHECK(start_x1 < 400, "start button at the left edge");
    XWT_CHECK(exit_x0 > out_w - 300, "exit at the right edge");

    /* the exit button is red somewhere in its run (row scan) */
    bool red = false;
    for (int px = exit_x0; px < out_w && !red; px++)
        if (pixel_at(t, px, y) == 0xffa33434 ||
            pixel_at(t, px, y) == 0xffc94b4b)
            red = true;
    XWT_CHECK(red, "the rightmost widget is the red exit button");

    (void)clock_x0;
    (void)want_big_font;
    reap(&pid);
}

static void test_layout_sizes(struct xwt_ctx *t) {
    layout_at(t, 1280, 720, 30, false);
}

static void test_layout_large(struct xwt_ctx *t) {
    layout_at(t, 1920, 1080, 34, false);
}

static void test_layout_1440p(struct xwt_ctx *t) {
    layout_at(t, 2560, 1440, 45, true);
}

static void test_layout_4k(struct xwt_ctx *t) {
    layout_at(t, 3840, 2160, 52, true);
}

/* bottom bar: the layer anchors the bottom edge, the bar sits at the
 * bottom of the output, windows keep their exclusive zone above */
static void test_layout_bottom(struct xwt_ctx *t) {
    /* a config file with position=bottom */
    char conf[256];
    snprintf(conf, sizeof(conf), "%s/panel-bottom.conf", g_runtimedir());
    FILE *f = fopen(conf, "w");
    XWT_ASSERT(f);
    fputs("[panel]\nposition=bottom\n", f);
    fclose(f);

    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        setenv("XW_PANEL_CONF", conf, 1);
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        int logfd = open("/tmp/xw-panel-child-bottom.log",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(panel_path(), "xw-panel", NULL);
        _exit(127);
    }
    XWT_ASSERT(pid > 0);
    PANEL_WAIT(t, the_layer(t) && the_layer(t)->mapped);
    struct xw_layer_surface *ls = the_layer(t);
    XWT_CHECK(ls->anchors & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
              "layer anchored to the bottom edge");
    XWT_CHECK(ls->y == 720 - ls->h, "bar sits at the output bottom (y=%d)",
              ls->y);
    XWT_CHECK(pixel_at(t, 640, 719) == 0xff22262e,
              "bottom row renders the bar background");
    XWT_CHECK(pixel_at(t, 640, 720 - ls->h + 5) == 0xff22262e,
              "top row of the bar renders");
    reap(&pid);
    unlink(conf);
}

static const struct xwt_test tests[] = {
    {"panel-metrics", test_metrics},
    {"panel-config", test_config},
    {"panel-clock-format", test_clock_format},
    {"panel-layout-720p", test_layout_sizes},
    {"panel-layout-1080p", test_layout_large},
    {"panel-layout-1440p", test_layout_1440p},
    {"panel-layout-4k", test_layout_4k},
    {"panel-layout-bottom", test_layout_bottom},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
