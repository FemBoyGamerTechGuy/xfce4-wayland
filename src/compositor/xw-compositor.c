/* xw-compositor — the display server binary.
 *
 * Parses arguments, builds the compositor configuration (outputs from
 * -o specs), runs the event loop and exits cleanly on SIGINT/SIGTERM
 * or xw_compositor_stop().
 *
 * Fatal-signal diagnostics: SIGSEGV/SIGBUS/SIGABRT/SIGFPE print the
 * fault address, the compositor state summary and a backtrace BEFORE
 * the process dies (re-raised with the default disposition, so the
 * session manager still observes a signaled exit). Without this, an
 * internal crash surfaces only as an opaque "restarting (1/3)" line in
 * the session log.
 */
#include "xw.h"
#include "xw-internal.h"

#include <execinfo.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct xw_compositor *g_comp;

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    /* one-shot guard: a fault inside the handler itself must not loop */
    static volatile sig_atomic_t in_crash = 0;
    if (in_crash)
        _exit(128 + sig);
    in_crash = 1;

    const char *why = "unknown";
    switch (sig) {
    case SIGSEGV: why = "segmentation fault"; break;
    case SIGBUS:  why = "bus error"; break;
    case SIGABRT: why = "abort (library assertion or NULL request handler)"; break;
    case SIGFPE:  why = "floating point exception"; break;
    }

    /* the message libwayland prints for a NULL request listener dies
     * with the process; make OUR message impossible to miss */
    fprintf(stderr,
            "\n[xw-fatal] xw-compositor caught signal %d (%s) at %s\n",
            sig, why,
            sig == SIGSEGV || sig == SIGBUS ? "(faulting address below)" : "");

    if (sig == SIGSEGV || sig == SIGBUS) {
        char addr[32];
        snprintf(addr, sizeof(addr), "%p", si->si_addr);
        fprintf(stderr, "[xw-fatal] fault address: %s (code %d)\n", addr,
                si->si_code);
    }
    if (si->si_code == SI_USER || si->si_code == SI_QUEUE)
        fprintf(stderr, "[xw-fatal] sent by pid %d, uid %d\n",
                (int)si->si_pid, (int)si->si_uid);

    xw_compositor_dump_state(g_comp);

    void *bt[40];
    int n = backtrace(bt, 40);
    if (n > 0) {
        fprintf(stderr, "[xw-fatal] backtrace (%d frames):\n", n);
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
    }
    fprintf(stderr, "[xw-fatal] dying now (default disposition re-raised; "
                    "the session manager will restart the compositor)\n");

    /* re-raise so the wait status stays WIFSIGNALED: honest crash,
     * never a masked silent exit */
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig); /* unreachable */
}

static void install_crash_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int fatal[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE};
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);
}

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "The xfce4-wayland compositor.\n"
           "\n"
           "Backends:\n"
           "  headless   no display hardware; deterministic tests/CI (default)\n"
           "  nested     a window inside a parent Wayland session ($WAYLAND_DISPLAY)\n"
           "  x11        a window inside an X11/XLibre session ($DISPLAY)\n"
           "  drm        physical display hardware (KMS; needs a seat manager)\n"
           "\n"
           "Seat providers (drm backend only):\n"
           "  auto       elogind/logind session through libseat if the\n"
           "             machine has one, then the seatd socket, then a\n"
           "             direct VT session (default)\n"
           "  elogind    libseat pinned to its logind backend (elogind\n"
           "             speaks the same org.freedesktop.login1 D-Bus API;\n"
           "             'logind' is an alias)\n"
           "  libseat    the external libseat library, whatever backend it\n"
           "             picks itself (it prefers seatd over logind)\n"
           "  seatd      the built-in seatd wire-protocol client\n"
           "  direct     take over the controlling tty directly\n"
           "\n"
           "Options:\n"
           "  -B, --backend NAME        headless | nested | x11 | drm\n"
           "  -P, --seat-provider NAME  auto | elogind | logind | libseat |\n"
           "                            seatd | direct\n"
           "  -I, --input MODE          auto | libinput | none (real input;\n"
           "                               auto only with $XW_INPUT_DEVICES or\n"
           "                               the drm backend)\n"
           "  -c, --config-dir DIR      configuration directory (INI files)\n"
           "  -s, --socket NAME         wayland socket name (default: auto)\n"
           "  -t, --seat NAME           seat name (default: seat0)\n"
           "  -o, --output SPEC         output spec WxH (repeatable; headless)\n"
           "  -D, --parent-display NAME parent display of the nested backend\n"
           "                               (WAYLAND_DISPLAY name or X display)\n"
           "  -q, --quiet               warnings only\n"
           "  -v, --verbose             debug logging\n"
           "  -h, --help                this help\n",
           prog);
}

static int parse_backend(const char *name) {
    if (!name || strcmp(name, "headless") == 0)
        return XW_BACKEND_HEADLESS;
    if (strcmp(name, "nested") == 0 || strcmp(name, "wayland") == 0)
        return XW_BACKEND_NESTED;
    if (strcmp(name, "x11") == 0 || strcmp(name, "X11") == 0)
        return XW_BACKEND_X11;
    if (strcmp(name, "drm") == 0 || strcmp(name, "DRM") == 0 ||
        strcmp(name, "kms") == 0)
        return XW_BACKEND_DRM;
    return -1;
}

static int parse_seat_provider(const char *name) {
    if (!name || strcmp(name, "auto") == 0)
        return XW_SEAT_PROVIDER_AUTO;
    if (strcmp(name, "elogind") == 0 || strcmp(name, "logind") == 0)
        return XW_SEAT_PROVIDER_ELOGIND;
    if (strcmp(name, "libseat") == 0)
        return XW_SEAT_PROVIDER_LIBSEAT;
    if (strcmp(name, "seatd") == 0)
        return XW_SEAT_PROVIDER_SEATD;
    if (strcmp(name, "direct") == 0)
        return XW_SEAT_PROVIDER_DIRECT;
    return -1;
}

static int parse_input(const char *name) {
    if (!name || strcmp(name, "auto") == 0)
        return XW_INPUT_AUTO;
    if (strcmp(name, "libinput") == 0)
        return XW_INPUT_LIBINPUT;
    if (strcmp(name, "none") == 0)
        return XW_INPUT_NONE;
    return -1;
}

int main(int argc, char **argv) {
    struct xw_compositor_config cfg = {0};
    struct xw_output_spec outputs[8];
    int n_outputs = 0;
    int backend = XW_BACKEND_HEADLESS;

    static const struct option longopts[] = {
        {"backend", required_argument, NULL, 'B'},
        {"seat-provider", required_argument, NULL, 'P'},
        {"input", required_argument, NULL, 'I'},
        {"config-dir", required_argument, NULL, 'c'},
        {"socket", required_argument, NULL, 's'},
        {"seat", required_argument, NULL, 't'},
        {"output", required_argument, NULL, 'o'},
        {"parent-display", required_argument, NULL, 'D'},
        {"quiet", no_argument, NULL, 'q'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "B:P:I:c:s:t:o:D:qvh", longopts,
                              NULL)) != -1) {
        switch (opt) {
        case 'B':
            backend = parse_backend(optarg);
            if (backend < 0) {
                fprintf(stderr,
                        "unknown backend '%s' (headless|nested|x11|drm)\n",
                        optarg);
                return 1;
            }
            break;
        case 'P':
            cfg.seat_provider = parse_seat_provider(optarg);
            if (cfg.seat_provider < 0) {
                fprintf(stderr,
                        "unknown seat provider '%s' "
                        "(auto|elogind|logind|libseat|seatd|direct)\n",
                        optarg);
                return 1;
            }
            break;
        case 'I':
            cfg.input_mode = parse_input(optarg);
            if (cfg.input_mode < 0) {
                fprintf(stderr,
                        "unknown input mode '%s' (auto|libinput|none)\n",
                        optarg);
                return 1;
            }
            break;
        case 'c':
            cfg.config_dir = optarg;
            break;
        case 's':
            cfg.socket_name = optarg;
            break;
        case 't':
            cfg.seat_name = optarg;
            break;
        case 'D':
            cfg.parent_display = optarg;
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
    cfg.backend = backend;
    /* $XW_SEAT_PROVIDER overrides the build default (auto) unless the
     * flag was given explicitly */
    if (!cfg.seat_provider) {
        const char *env = getenv("XW_SEAT_PROVIDER");
        if (env && *env) {
            int p = parse_seat_provider(env);
            if (p > 0)
                cfg.seat_provider = p;
            else
                fprintf(stderr,
                        "ignoring unknown $XW_SEAT_PROVIDER '%s'\n", env);
        }
    }
    if (backend == XW_BACKEND_DRM && !cfg.seat_provider)
        cfg.seat_provider = XW_SEAT_PROVIDER_AUTO;
    if (n_outputs > 0) {
        cfg.outputs = outputs;
        cfg.n_outputs = n_outputs;
    }

    xw_log_set_level(cfg.log_level);
    install_crash_handlers();

    struct xw_compositor *c = xw_compositor_create(&cfg);
    if (!c) {
        fprintf(stderr, "xw-compositor: failed to start\n");
        return 1;
    }
    g_comp = c;
    printf("%s\n", xw_compositor_socket_path(c));
    fflush(stdout);

    int code = xw_compositor_run(c);
    g_comp = NULL;
    xw_compositor_destroy(c);

    for (int i = 0; i < n_outputs; i++)
        free((void *)outputs[i].name);
    return code;
}
