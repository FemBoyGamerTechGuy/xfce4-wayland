/* xw-power.c — logind/elogind power backend (see xw-power.h). */
#include "xw-power.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Does loginctl run and reach a live logind/elogind? A loginctl
 * without a running daemon answers "Failed to connect to bus" and a
 * non-zero exit — that must read as unavailable, not as present. */
static bool loginctl_alive(void) {
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char *const argv[] = {"loginctl", "list-sessions", NULL};
        execvp(argv[0], argv);
        _exit(127); /* not found */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* /sys/power/state contains the kernel's supported sleep modes as
 * whitespace-separated tokens ("freeze mem disk"). */
static bool kernel_sleep_modes(bool *suspend, bool *hibernate,
                               char *seen, size_t seen_len) {
    *suspend = false;
    *hibernate = false;
    seen[0] = 0;
    const char *path = getenv("XW_POWER_STATE_PATH");
    if (!path || !*path)
        path = "/sys/power/state";
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char tok[32];
    while (fscanf(f, "%31s", tok) == 1) {
        if (seen_len - strlen(seen) > strlen(tok) + 2) {
            if (seen[0])
                strncat(seen, " ", seen_len - strlen(seen) - 1);
            strncat(seen, tok, seen_len - strlen(seen) - 1);
        }
        if (strcmp(tok, "mem") == 0 || strcmp(tok, "freeze") == 0)
            *suspend = true;
        if (strcmp(tok, "disk") == 0)
            *hibernate = true;
    }
    fclose(f);
    return true;
}

bool xw_power_probe(struct xw_power_caps *caps) {
    memset(caps, 0, sizeof(*caps));

    caps->loginctl_ok = loginctl_alive();

    bool ks = false, kh = false;
    char modes[64] = {0};
    bool have_modes = kernel_sleep_modes(&ks, &kh, modes, sizeof(modes));

    caps->suspend = caps->loginctl_ok && ks;
    caps->hibernate = caps->loginctl_ok && kh;
    caps->poweroff = caps->loginctl_ok;
    caps->reboot = caps->loginctl_ok;

    if (!caps->loginctl_ok)
        snprintf(caps->suspend_reason, sizeof(caps->suspend_reason),
                 "no working logind/elogind (loginctl)");
    else if (!have_modes)
        snprintf(caps->suspend_reason, sizeof(caps->suspend_reason),
                 "cannot read /sys/power/state");
    else if (!ks)
        snprintf(caps->suspend_reason, sizeof(caps->suspend_reason),
                 "kernel suspend unsupported (%s)", modes);

    if (!caps->loginctl_ok)
        snprintf(caps->hibernate_reason, sizeof(caps->hibernate_reason),
                 "no working logind/elogind (loginctl)");
    else if (!have_modes)
        snprintf(caps->hibernate_reason, sizeof(caps->hibernate_reason),
                 "cannot read /sys/power/state");
    else if (!kh)
        snprintf(caps->hibernate_reason, sizeof(caps->hibernate_reason),
                 "kernel hibernation unsupported (%s)", modes);

    snprintf(caps->poweroff_reason, sizeof(caps->poweroff_reason),
             "no working logind/elogind (loginctl)");
    snprintf(caps->reboot_reason, sizeof(caps->reboot_reason),
             "no working logind/elogind (loginctl)");
    return true;
}

bool xw_power_exec(const char *verb, char *err, size_t errlen) {
    if (err && errlen)
        err[0] = 0;
    if (!verb || !*verb)
        return false;

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        /* capture the backend's stderr for the error path */
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0)
            dup2(devnull, STDOUT_FILENO);
        char *const argv[] = {"loginctl", (char *)verb, NULL};
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);

    /* read a bounded slice of stderr (first line wins) */
    char buf[256] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    (void)n;
    close(pipefd[0]);

    int status = 0;
    bool ok = waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0;
    if (!ok && err && errlen) {
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = 0;
        for (char *p = buf; *p; p++) /* keep it printable */
            if ((unsigned char)*p < ' ')
                *p = ' ';
        if (buf[0])
            snprintf(err, errlen, "loginctl: %s", buf);
        else if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            snprintf(err, errlen, "loginctl not found");
        else
            snprintf(err, errlen, "loginctl failed (exit %d)",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return ok;
}
