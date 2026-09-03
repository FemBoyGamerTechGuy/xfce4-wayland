/* xwtest.h — in-process integration test harness.
 *
 * Each test embeds the compositor (libxw) and connects a real wayland
 * client (libxwcl) over the socket, then pumps both sides in a
 * deterministic loop. Server-side state is asserted white-box; client
 * state is asserted from the test client.
 */
#ifndef XWTEST_H
#define XWTEST_H

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xw.h"
#include "xw-internal.h"
#include "xwc.h"
#include "wayland-client.h"

/* linux keycodes (input-event-codes.h values, spelled out for clarity) */
#define K_ESC 1
#define K_TAB 15
#define K_F1 59
#define K_F2 60
#define K_F3 61
#define K_F4 62
#define K_F6 64
#define K_F7 65
#define K_F8 66
#define K_F9 67
#define K_F10 68
#define K_F11 87
#define K_F12 88
#define K_NUMLOCK 69
#define K_LEFTCTRL 29
#define K_RIGHTCTRL 97
#define K_LEFTSHIFT 42
#define K_LEFTALT 56
#define K_LEFTMETA 125 /* Super */
#define K_DELETE 111
#define K_INSERT 110
#define K_HOME 102
#define K_END 107
#define K_PAGEUP 104
#define K_PAGEDOWN 109
#define K_UP 103
#define K_DOWN 108
#define K_LEFT 105
#define K_RIGHT 106
#define K_ENTER 28
#define K_D 32
#define K_L 38
#define K_T 20

extern int xwt_failures;
extern int xwt_tests;
extern const char *xwt_current;

#define XWT_CHECK(cond, ...)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            xwt_failures++;                                                   \
            printf("  FAIL %s:%d: %s — ", xwt_current, __LINE__, #cond);      \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define XWT_ASSERT(cond)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            xwt_failures++;                                                   \
            printf("  FAIL %s:%d: assertion %s\n", xwt_current, __LINE__,     \
                   #cond);                                                    \
            return;                                                           \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------- harness */

struct xwt_ctx {
    struct xw_compositor *comp;
    struct xwc client;
    char socket_name[32];
    bool client_dead; /* in-process client hit a connection/protocol
                         error; the pump stops touching its display */
};

/* Starts a compositor with a unique socket and connects a client.
 * Returns 0 on success; on failure the test should abort. */
int xwt_begin(struct xwt_ctx *t, const char *config_dir);
void xwt_end(struct xwt_ctx *t);

/* One pump cycle: server dispatch (non-blocking) + client read (non-
 * blocking). Call in a loop until a condition holds. */
void xwt_pump(struct xwt_ctx *t);

/* Pump until cond is true (evaluated after each pump), or fail after
 * n iterations. */
#define XWT_WAIT(t, cond)                                                     \
    ({                                                                        \
        bool _ok = false;                                                     \
        for (int _i = 0; _i < 2000 && !(_ok = (cond)); _i++)                  \
            xwt_pump(t);                                                      \
        XWT_CHECK(_ok, "timeout waiting for: %s", #cond);                     \
        _ok;                                                                  \
    })

const char *g_runtimedir(void);

/* test registration */
struct xwt_test {
    const char *name;
    void (*fn)(struct xwt_ctx *t);
};

void xwt_register(const struct xwt_test *tests, int n);
int xwt_run_all(void);

/* create a window that fills itself with a solid color and commits */
struct xwc_win *xwt_window_solid(struct xwt_ctx *t, uint32_t color, int w,
                                 int h, const char *title);
/* bind wl_subcompositor on a test client (raw core-protocol tests) */
void xwt_bind_subcompositor(struct xwt_ctx *t, struct wl_subcompositor **out);

#endif /* XWTEST_H */
