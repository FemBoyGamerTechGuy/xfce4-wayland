/* test_drm.c — DRM/KMS planning logic (Phase 4).
 *
 * Exercises the pure planning functions of xw-backend-drm.c — mode
 * selection, connector naming and CRTC/encoder assignment — with plain
 * struct tables and no hardware. The ioctl paths (dumb-buffer scanout,
 * page flips, modeset/restore, hotplug) are covered by the manual
 * hardware checklist in TESTING.md: they need a real GPU and monitor
 * and cannot be faked honestly in CI.
 */
#include "xwtest.h"

#include <string.h>

static void test_pick_mode_preferred(struct xwt_ctx *t) {
    (void)t;
    struct xw_drm_mode modes[] = {
        {1920, 1080, 60, 0},
        {1280, 720, 60, 0x40}, /* preferred */
        {640, 480, 60, 0},
    };
    const struct xw_drm_mode *m = xw_drm_pick_mode(modes, 3);
    XWT_CHECK(m == &modes[1], "the preferred mode wins over larger modes");

    /* several preferred: largest preferred wins */
    struct xw_drm_mode multi[] = {
        {1920, 1080, 60, 0x40},
        {3840, 2160, 30, 0x40},
        {2560, 1440, 60, 0x40},
    };
    m = xw_drm_pick_mode(multi, 3);
    XWT_CHECK(m == &multi[1], "largest of several preferred modes wins");
}

static void test_pick_mode_largest(struct xwt_ctx *t) {
    (void)t;
    struct xw_drm_mode modes[] = {
        {640, 480, 60, 0},
        {1920, 1080, 60, 0},
        {1280, 1024, 75, 0},
    };
    const struct xw_drm_mode *m = xw_drm_pick_mode(modes, 3);
    XWT_CHECK(m == &modes[1], "without a preferred flag, largest area wins");
}

static void test_pick_mode_refresh_tiebreak(struct xwt_ctx *t) {
    (void)t;
    struct xw_drm_mode modes[] = {
        {1280, 720, 60, 0},
        {1280, 720, 75, 0},
        {1280, 720, 50, 0},
    };
    const struct xw_drm_mode *m = xw_drm_pick_mode(modes, 3);
    XWT_CHECK(m == &modes[1],
              "equal areas pick the higher refresh rate");
}

static void test_pick_mode_empty(struct xwt_ctx *t) {
    (void)t;
    XWT_CHECK(xw_drm_pick_mode(NULL, 0) == NULL, "no modes -> NULL");
    XWT_CHECK(xw_drm_pick_mode(NULL, 5) == NULL, "NULL table -> NULL");
    struct xw_drm_mode one[] = {{1024, 768, 60, 0}};
    XWT_CHECK(xw_drm_pick_mode(one, 1) == &one[0],
              "a single mode is selected trivially");
}

static void test_connector_names(struct xwt_ctx *t) {
    (void)t;
    char buf[32];
    xw_drm_connector_name(11, 1, buf, sizeof(buf)); /* HDMI-A */
    XWT_CHECK(strcmp(buf, "HDMI-A-1") == 0, "HDMI-A-1 (got %s)", buf);
    xw_drm_connector_name(10, 3, buf, sizeof(buf)); /* DisplayPort */
    XWT_CHECK(strcmp(buf, "DP-3") == 0, "DP-3 (got %s)", buf);
    xw_drm_connector_name(14, 1, buf, sizeof(buf)); /* eDP */
    XWT_CHECK(strcmp(buf, "eDP-1") == 0, "eDP-1 (got %s)", buf);
    xw_drm_connector_name(1, 2, buf, sizeof(buf)); /* VGA */
    XWT_CHECK(strcmp(buf, "VGA-2") == 0, "VGA-2 (got %s)", buf);
    xw_drm_connector_name(0, 5, buf, sizeof(buf));
    XWT_CHECK(strcmp(buf, "Unknown-5") == 0, "Unknown-5 (got %s)", buf);
    XWT_CHECK(strcmp(xw_drm_connector_type_name(12), "HDMI-B") == 0,
              "HDMI-B type name");
    XWT_CHECK(strcmp(xw_drm_connector_type_name(7), "LVDS") == 0,
              "LVDS type name");
}

static void test_plan_crtc_firmware_pairing(struct xwt_ctx *t) {
    (void)t;
    /* the firmware's own encoder->crtc pairing is reused when free */
    int crtcs[3] = {42, 43, 44};
    bool taken[3] = {false, false, false};
    int idx = xw_drm_plan_crtc(9 /*enc*/, 43 /*enc's crtc*/, 0x7, crtcs, 3,
                               taken);
    XWT_CHECK(idx == 1, "firmware pairing picks crtc index 1 (got %d)", idx);
    XWT_CHECK(taken[1], "the chosen crtc is marked taken");
    XWT_CHECK(!taken[0] && !taken[2], "others stay free");

    /* a taken firmware pairing falls through to the possible mask */
    bool taken2[3] = {false, true, false};
    idx = xw_drm_plan_crtc(9, 43, 0x4, crtcs, 3, taken2);
    XWT_CHECK(idx == 2, "falls back to a possible crtc (got %d)", idx);
}

static void test_plan_crtc_possible_mask(struct xwt_ctx *t) {
    (void)t;
    int crtcs[4] = {50, 51, 52, 53};
    bool taken[4] = {true, false, true, false};
    /* possible mask 0b1010 = crtcs 1 and 3; 1 is free */
    int idx = xw_drm_plan_crtc(0, 0, 0xA, crtcs, 4, taken);
    XWT_CHECK(idx == 1, "first free possible crtc (got %d)", idx);
}

static void test_plan_crtc_exhausted(struct xwt_ctx *t) {
    (void)t;
    int crtcs[2] = {60, 61};
    bool taken[2] = {true, true};
    XWT_CHECK(xw_drm_plan_crtc(0, 0, 0x3, crtcs, 2, taken) == -1,
              "all crtcs taken -> -1");
    XWT_CHECK(xw_drm_plan_crtc(0, 0, 0x0, crtcs, 2, taken) == -1,
              "empty possible mask -> -1");
    bool free[2] = {false, false};
    XWT_CHECK(xw_drm_plan_crtc(0, 0, 0x0, crtcs, 2, free) == -1,
              "no encoder capabilities -> -1");
}

__attribute__((constructor)) static void register_drm(void) {
    static const struct xwt_test tests[] = {
        {"drm-pick-mode-preferred", test_pick_mode_preferred},
        {"drm-pick-mode-largest", test_pick_mode_largest},
        {"drm-pick-mode-refresh-tiebreak", test_pick_mode_refresh_tiebreak},
        {"drm-pick-mode-empty", test_pick_mode_empty},
        {"drm-connector-names", test_connector_names},
        {"drm-plan-crtc-firmware-pairing", test_plan_crtc_firmware_pairing},
        {"drm-plan-crtc-possible-mask", test_plan_crtc_possible_mask},
        {"drm-plan-crtc-exhausted", test_plan_crtc_exhausted},
    };
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
