/* trace_intent.c — LD_PRELOAD interposer for libwayland-client's
 * read-intent functions. Logs each call + caller to stderr so a
 * dangling prepare_read can be attributed to its exact call site. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

static int (*real_prepare)(void *);
static void (*real_cancel)(void *);
static int (*real_read)(void *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int depth = 0;

static void logop(const char *op, void *d, void *caller) {
    pthread_mutex_lock(&lock);
    fprintf(stderr, "[INTENT %ld] %s display=%p from %p\n",
            (long)syscall(SYS_gettid), op, d, caller);
    fflush(stderr);
    pthread_mutex_unlock(&lock);
}

int wl_display_prepare_read(void *display) {
    if (!real_prepare)
        real_prepare = dlsym(RTLD_NEXT, "wl_display_prepare_read");
    int r = real_prepare(display);
    if (depth < 8) {
        depth++;
        logop(r == 0 ? "prepare(OK)" : "prepare(refused)", display,
              __builtin_return_address(0));
        depth--;
    }
    return r;
}

void wl_display_cancel_read(void *display) {
    if (!real_cancel)
        real_cancel = dlsym(RTLD_NEXT, "wl_display_cancel_read");
    real_cancel(display);
    logop("cancel", display, __builtin_return_address(0));
}

int wl_display_read_events(void *display) {
    if (!real_read)
        real_read = dlsym(RTLD_NEXT, "wl_display_read_events");
    logop("read-events:ENTER", display, __builtin_return_address(0));
    int r = real_read(display);
    logop(r == 0 ? "read-events:OK" : "read-events:ERR", display, NULL);
    return r;
}
