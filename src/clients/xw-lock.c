/* xw-lock.c — the xfce4-wayland session lock client.
 *
 * Locks the session via ext-session-lock-v1 and displays a passphrase
 * prompt. The compositor blanks everything else and routes all input
 * here while locked (server-enforced; see xw-session-lock.c).
 *
 * Authentication (v0, honest scope): a local passphrase file. This is
 * development-grade, NOT PAM — documented in README/ROADMAP as the
 * interim authenticator until the PAM unlock backend lands. Without a
 * passphrase file xw-lock refuses to start: a lock that can never be
 * unlocked is worse than no lock.
 *
 * Usage:
 *   xw-lock [--passphrase-file PATH] [--idle SECONDS]
 *
 *   --passphrase-file PATH   file whose first line is the passphrase
 *                            (default: $XW_LOCK_PASSPHRASE_FILE, then
 *                            ~/.config/xfce4-wayland/lock-pass)
 *   --idle SECONDS           lock after SECONDS without input
 *                            (ext-idle-notify; default: lock at once)
 *
 * Exit codes: 0 unlocked (passphrase accepted), 1 error / connection
 * lost / lock denied, 2 usage.
 *
 * SIGINT/SIGTERM exit WITHOUT unlocking: only the passphrase (or
 * session termination) unlocks — killing the locker must not unlock
 * the session (the compositor keeps the session locked if this client
 * dies, per protocol).
 */
#include "xwc.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PASS 128

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* logging via stderr (visible in the session exit log) */
static void xwc_log_info(const char *msg) { fprintf(stderr, "%s\n", msg); }

/* ------------------------------------------------------------ passphrase */

static char *g_secret; /* first line of the passphrase file, trimmed */

static bool load_passphrase(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[MAX_PASS + 2];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0])
        return false;
    g_secret = strdup(line);
    return g_secret != NULL;
}

/* constant-time comparison (no early exit on mismatch) */
static bool passphrase_ok(const char *input) {
    size_t a = strlen(input), b = strlen(g_secret);
    volatile unsigned char diff = (unsigned char)(a != b);
    size_t n = a > b ? a : b;
    for (size_t i = 0; i < n; i++) {
        unsigned char x = i < a ? (unsigned char)input[i] : 0;
        unsigned char y = i < b ? (unsigned char)g_secret[i] : 0;
        diff |= (unsigned char)(x ^ y);
    }
    return diff == 0;
}

/* ------------------------------------------------------------------ UI */

static struct xwc g_conn;
static struct xwc_lock *g_lock;
static bool g_want_lock; /* idle fired: create the lock in the main loop */
static bool g_unlocked;

static char g_input[MAX_PASS];
static size_t g_input_len;
static char g_status[64];

enum {
    COL_BG = 0xff10151c,
    COL_PANEL = 0xff1c2733,
    COL_BORDER = 0xff46586b,
    COL_TITLE = 0xffffffff,
    COL_TEXT = 0xffc8d3e0,
    COL_DIM = 0xff7f8c9a,
    COL_ERR = 0xffff6b6b,
    COL_DOT = 0xff5ec4ff,
};

static void draw(struct xwc_lock *l) {
    int w = 0, h = 0, stride = 0;
    xwc_lock_size(l, &w, &h);
    uint32_t *pix = xwc_lock_pixels(l, &stride);
    if (!pix || w < 64 || h < 64)
        return;

    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, COL_BG);

    int bw = 420, bh = 170;
    if (bw > w - 16)
        bw = w - 16;
    if (bh > h - 16)
        bh = h - 16;
    int bx = (w - bw) / 2, by = (h - bh) / 2;
    xwc_draw_box(pix, stride, w, h, bx, by, bw, bh, COL_PANEL, COL_BORDER);

    const char *title = "xfce4-wayland";
    int tx = bx + (bw - xwc_text_width(title)) / 2;
    xwc_draw_text(pix, stride, w, h, tx, by + 18, title, COL_TITLE);

    const char *sub = "Session locked";
    int sx = bx + (bw - xwc_text_width(sub)) / 2;
    xwc_draw_text(pix, stride, w, h, sx, by + 44, sub, COL_DIM);

    const char *prompt = "Passphrase:";
    xwc_draw_text(pix, stride, w, h, bx + 24, by + 82, prompt, COL_TEXT);

    /* masked input: one dot per character */
    char dots[MAX_PASS + 1];
    size_t n = g_input_len < MAX_PASS ? g_input_len : MAX_PASS;
    memset(dots, '*', n);
    dots[n] = '\0';
    if (n == 0)
        dots[0] = '_', dots[1] = '\0';
    xwc_draw_text(pix, stride, w, h, bx + 24 + xwc_text_width(prompt) + 10,
                  by + 82, dots, COL_DOT);

    if (g_status[0]) {
        int stx = bx + (bw - xwc_text_width(g_status)) / 2;
        uint32_t col = COL_ERR;
        xwc_draw_text(pix, stride, w, h, stx, by + bh - 34, g_status, col);
    } else {
        const char *hint = "Enter to unlock";
        int hx = bx + (bw - xwc_text_width(hint)) / 2;
        xwc_draw_text(pix, stride, w, h, hx, by + bh - 34, hint, COL_DIM);
    }

    xwc_lock_commit(l);
}

static void redraw_locked(void) {
    if (g_lock)
        draw(g_lock);
}

/* ------------------------------------------------------------ callbacks */

static void on_lock_configure(struct xwc_lock *l, int w, int h, void *ud) {
    (void)w;
    (void)h;
    (void)ud;
    /* fires during xwc_lock_create (before g_lock is assigned): use
     * the lock parameter, not the global */
    draw(l);
}

static void on_lock_locked(struct xwc_lock *l, void *ud) {
    (void)l;
    (void)ud;
    xwc_log_info("xw-lock: session locked");
}
static void on_lock_finished(struct xwc_lock *l, void *ud) {
    (void)l;
    (void)ud;
    fprintf(stderr, "xw-lock: lock denied (another client holds the lock)\n");
    g_stop = 1; /* exit code 1 via the error path below */
}

static void on_lock_key(struct xwc_lock *l, uint32_t keycode, bool down,
                        xkb_keysym_t keysym, uint32_t mods, void *ud) {
    (void)l;
    (void)keycode;
    (void)mods;
    (void)ud;
    if (!down || !g_lock)
        return;

    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
        if (passphrase_ok(g_input)) {
            g_unlocked = true;
            g_stop = 1;
        } else {
            snprintf(g_status, sizeof(g_status), "incorrect passphrase");
            g_input_len = 0;
            g_input[0] = '\0';
            redraw_locked();
        }
        return;
    }
    if (keysym == XKB_KEY_BackSpace) {
        if (g_input_len > 0)
            g_input[--g_input_len] = '\0';
        redraw_locked();
        return;
    }
    if (keysym == XKB_KEY_Escape) {
        g_input_len = 0;
        g_input[0] = '\0';
        g_status[0] = '\0';
        redraw_locked();
        return;
    }

    uint32_t cp = xkb_keysym_to_utf32(keysym);
    if (cp >= 0x20 && cp < 0x7f && g_input_len + 1 < sizeof(g_input)) {
        g_input[g_input_len++] = (char)cp;
        g_input[g_input_len] = '\0';
        if (g_status[0]) {
            g_status[0] = '\0';
        }
        redraw_locked();
    }
}

static void on_idle_idled(void *ud) {
    (void)ud;
    if (!g_lock && !g_want_lock) {
        g_want_lock = true; /* create the lock in the main loop (no
                               re-entrant dispatch inside a handler) */
        xwc_log_info("xw-lock: idle timeout reached, locking");
    }
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *out) {
    fprintf(out,
            "usage: xw-lock [--passphrase-file PATH] [--idle SECONDS]\n"
            "  --passphrase-file PATH  first line = passphrase (default:\n"
            "                         $XW_LOCK_PASSPHRASE_FILE or\n"
            "                         ~/.config/xfce4-wayland/lock-pass)\n"
            "  --idle SECONDS          lock after SECONDS without input\n");
}

int main(int argc, char **argv) {
    const char *pass_file = NULL;
    double idle_sec = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--passphrase-file") == 0 && i + 1 < argc) {
            pass_file = argv[++i];
        } else if (strcmp(argv[i], "--idle") == 0 && i + 1 < argc) {
            idle_sec = atof(argv[++i]);
            if (idle_sec < 0) {
                usage(stderr);
                return 2;
            }
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (!pass_file) {
        const char *env = getenv("XW_LOCK_PASSPHRASE_FILE");
        if (env && *env) {
            pass_file = env;
        } else {
            const char *home = getenv("HOME");
            char path[512];
            if (home && (size_t)snprintf(path, sizeof(path),
                                         "%s/.config/xfce4-wayland/lock-pass",
                                         home) < sizeof(path)) {
                if (access(path, F_OK) == 0)
                    pass_file = path;
            }
        }
    }
    if (!pass_file || !load_passphrase(pass_file)) {
        fprintf(stderr,
                "xw-lock: no usable passphrase file.\n"
                "  Create one:  printf 'secret\\n' > "
                "~/.config/xfce4-wayland/lock-pass\n"
                "  (chmod 600 it). Authentication is currently a local\n"
                "  passphrase file, not PAM — see ROADMAP.md.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (xwc_connect(&g_conn, NULL) != 0) {
        fprintf(stderr, "xw-lock: cannot connect to the compositor\n");
        return 1;
    }


    struct xwc_lock_cbs lcbs = {
        .locked = on_lock_locked,
        .finished = on_lock_finished,
        .configure = on_lock_configure,
        .key = on_lock_key,
        .ud = NULL,
    };

    struct xwc_idle *idle = NULL;
    if (idle_sec >= 0) {
        idle = xwc_idle_create(&g_conn, (uint32_t)(idle_sec * 1000.0),
                               on_idle_idled, NULL, NULL);
        if (!idle) {
            fprintf(stderr, "xw-lock: idle notification unavailable\n");
            xwc_disconnect(&g_conn);
            return 1;
        }
    }

    if (idle_sec < 0) {
        g_lock = xwc_lock_create(&g_conn, &lcbs);
            if (!g_lock) {
            fprintf(stderr, "xw-lock: session lock unavailable\n");
            xwc_idle_destroy(idle);
            xwc_disconnect(&g_conn);
            return 1;
        }
    }

    int rc = 0;
    while (!g_stop) {
        if (g_want_lock && !g_lock) {
            g_want_lock = false;
            g_lock = xwc_lock_create(&g_conn, &lcbs);
            if (!g_lock) {
                fprintf(stderr, "xw-lock: session lock unavailable\n");
                rc = 1;
                break;
            }
        }
        if (xwc_dispatch(&g_conn, 250) < 0) {
            /* connection lost while locked: the session STAYS locked
             * (compositor policy); we exit non-zero */
            if (g_lock && xwc_lock_locked(g_lock)) {
                fprintf(stderr, "xw-lock: connection lost while locked; "
                                "the session remains locked\n");
                rc = 1;
            }
            break;
        }
    }

    if (g_unlocked && g_lock) {
        xwc_lock_unlock(g_lock);
        rc = 0;
    }
    if (g_lock) {
        /* if not unlocked (signal or error): destroying the surfaces is
         * fine (the compositor falls back to blank per spec) but the
         * lock OBJECT must not be destroyed while it holds the lock
         * (protocol error) — xwc_lock_destroy handles that case: the
         * object dies with the connection, the session stays locked. */
        xwc_lock_destroy(g_lock);
        g_lock = NULL;
        if (!g_unlocked && rc == 0)
            rc = 1;
    }

    /* zero the passphrase before freeing */
    if (g_secret) {
        memset(g_secret, 0, strlen(g_secret));
        free(g_secret);
    }
    xwc_idle_destroy(idle);
    xwc_disconnect(&g_conn);
    return rc;
}
