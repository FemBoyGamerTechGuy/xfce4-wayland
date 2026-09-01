/* xw-backend-headless.c — deterministic headless backend.
 *
 * Owns output creation from the configuration. There is no real display
 * device involved; frame timing is damage-driven (repaint scheduled
 * whenever damage arrives), which keeps tests fully deterministic.
 * The synthetic input entry points (xw_compositor_inject_*) live in
 * xw-compositor.c and route through the seat like any backend would.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

static const struct xw_backend_ops headless_ops = {
    .present = NULL, /* pixels stay in the native buffer (introspection) */
    .destroy = NULL, /* outputs are freed by the generic teardown */
};

struct xw_backend *xw_backend_headless_create(struct xw_compositor *c,
                                              const struct xw_compositor_config *cfg) {
    struct xw_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->comp = c;
    b->name = "headless";
    b->ops = &headless_ops;

    int n = cfg->n_outputs > 0 ? cfg->n_outputs : 1;
    const struct xw_output_spec *specs =
        cfg->outputs ? cfg->outputs : NULL;

    int layout_x = 0;
    for (int i = 0; i < n; i++) {
        char name[64];
        if (specs && specs[i].name && *specs[i].name) {
            snprintf(name, sizeof(name), "%s", specs[i].name);
        } else {
            snprintf(name, sizeof(name), "HEADLESS-%d", i + 1);
        }
        int w = specs ? (specs[i].width > 0 ? specs[i].width : 1280) : 1280;
        int h = specs ? (specs[i].height > 0 ? specs[i].height : 720) : 720;
        int scale = specs ? (specs[i].scale > 0 ? specs[i].scale : 1) : 1;
        if (!xw_output_create(c, name, layout_x, 0, w, h, scale))
            goto fail;
        layout_x += w;
    }
    return b;
fail:
    xw_log(XW_LOG_ERROR, "headless backend init failed");
    xw_backend_destroy_outputs(c);
    free(b);
    return NULL;
}
