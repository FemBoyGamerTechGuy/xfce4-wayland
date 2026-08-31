/* debug-readevents.c — isolate the wl_display_read_events closure leak
 * seen in the asan child check.  Binds wl_output without a listener
 * (the server then emits geometry/mode/scale/done), reads the batch
 * with the xwc_dispatch poll pattern, and optionally SKIPS
 * dispatch_pending to test whether wl_display_disconnect frees queued
 * closures.  Build with the sanitizer flags from run-asan.sh.
 *
 * Modes: "" (dispatch, control) | "skip" (leave closures queued)
 */
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wayland-client.h"

static uint32_t g_output_name;
static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)r;
    (void)version;
    if (strcmp(iface, "wl_output") == 0)
        g_output_name = name;
}
static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
};

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    bool skip_dispatch = argc > 2 && strcmp(argv[2], "skip") == 0;
    struct wl_display *d = wl_display_connect(argv[1]);
    if (!d)
        return 3;
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_listener, NULL);
    wl_display_roundtrip(d);
    if (!g_output_name)
        return 5;

    /* bind with NO listener: the server immediately queues
     * geometry/mode/scale/done events for us */
    struct wl_output *out =
        wl_registry_bind(r, g_output_name, &wl_output_interface, 1);

    while (wl_display_prepare_read(d) != 0)
        wl_display_dispatch_pending(d);
    wl_display_flush(d);
    struct pollfd pfd = {.fd = wl_display_get_fd(d), .events = POLLIN};
    poll(&pfd, 1, 100);
    if (pfd.revents & POLLIN) {
        if (wl_display_read_events(d) < 0)
            return 4;
    } else {
        wl_display_cancel_read(d);
        printf("note: no events arrived\n");
    }
    if (!skip_dispatch)
        wl_display_dispatch_pending(d);

    wl_output_destroy(out);
    wl_registry_destroy(r);
    wl_display_disconnect(d);
    return 0;
}
