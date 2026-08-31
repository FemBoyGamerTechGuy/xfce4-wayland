/* xw-compositor — the display server binary.
 *
 * Parses arguments, builds the compositor configuration (outputs from
 * -o specs), runs the event loop and exits cleanly on SIGINT/SIGTERM
 * or xw_compositor_stop().
 */
#include "xw.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "The xfce4-wayland compositor (headless backend).\n"
           "\n"
           "Options:\n"
           "  -c, --config-dir DIR   configuration directory (INI files)\n"
           "  -s, --socket NAME      wayland socket name (default: auto)\n"
           "  -o, --output SPEC      output spec WxH (repeatable, e.g. -o 1280x720)\n"
           "  -q, --quiet            warnings only\n"
           "  -v, --verbose          debug logging\n"
           "  -h, --help             this help\n",
           prog);
}

int main(int argc, char **argv) {
    struct xw_compositor_config cfg = {0};
    struct xw_output_spec outputs[8];
    int n_outputs = 0;

    static const struct option longopts[] = {
        {"config-dir", required_argument, NULL, 'c'},
        {"socket", required_argument, NULL, 's'},
        {"output", required_argument, NULL, 'o'},
        {"quiet", no_argument, NULL, 'q'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:s:o:qvh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cfg.config_dir = optarg;
            break;
        case 's':
            cfg.socket_name = optarg;
            break;
        case 'o': {
            if (n_outputs >= 8) {
                fprintf(stderr, "too many outputs (max 8)\n");
                return 1;
            }
            int w = 0, h = 0;
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w < 16 || h < 16) {
                fprintf(stderr, "invalid output spec '%s' (want WxH)\n",
                        optarg);
                return 1;
            }
            char name[32];
            snprintf(name, sizeof(name), "HEADLESS-%d", n_outputs + 1);
            outputs[n_outputs].name = strdup(name);
            outputs[n_outputs].width = w;
            outputs[n_outputs].height = h;
            outputs[n_outputs].scale = 1;
            n_outputs++;
            break;
        }
        case 'q':
            cfg.log_level = XW_LOG_WARN;
            break;
        case 'v':
            cfg.log_level = XW_LOG_DEBUG;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.log_level == 0)
        cfg.log_level = XW_LOG_INFO;
    if (n_outputs > 0) {
        cfg.outputs = outputs;
        cfg.n_outputs = n_outputs;
    }

    xw_log_set_level(cfg.log_level);

    struct xw_compositor *c = xw_compositor_create(&cfg);
    if (!c) {
        fprintf(stderr, "xw-compositor: failed to start\n");
        return 1;
    }
    printf("%s\n", xw_compositor_socket_path(c));
    fflush(stdout);

    int code = xw_compositor_run(c);
    xw_compositor_destroy(c);

    for (int i = 0; i < n_outputs; i++)
        free((void *)outputs[i].name);
    return code;
}
