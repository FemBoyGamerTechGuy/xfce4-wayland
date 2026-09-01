/* xw-power.h — power backend shared by xw-session and xw-exit.
 *
 * Abstracts logind/elogind power management behind one interface:
 *  - capability probing with human-readable reasons (the exit dialog
 *    shows WHY an action is unavailable instead of silently failing),
 *  - execution through fork + execvp with an explicit argv and no
 *    shell, with the backend's stderr captured for diagnostics.
 *
 * Works with systemd-logind and elogind alike: both ship `loginctl`,
 * and the probing checks that loginctl can actually reach a running
 * daemon (a loginctl binary without a running logind is not enough).
 */
#ifndef XW_POWER_H
#define XW_POWER_H

#include <stdbool.h>
#include <stddef.h>

struct xw_power_caps {
    /* loginctl exists AND a logind/elogind daemon answers it */
    bool loginctl_ok;
    bool suspend;
    bool hibernate;
    bool poweroff;
    bool reboot;
    /* human-readable reasons, valid when the flag is false */
    char suspend_reason[128];
    char hibernate_reason[128];
    char poweroff_reason[128];
    char reboot_reason[128];
};

/* Probe capabilities. Never blocks long: loginctl fails fast when no
 * daemon answers. /sys/power/state is read from
 * $XW_POWER_STATE_PATH when set (testing/debug override), else
 * /sys/power/state. Returns true when the probe itself could run. */
bool xw_power_probe(struct xw_power_caps *caps);

/* Run `loginctl <verb>` without a shell (verb is one of the fixed
 * strings "poweroff"/"reboot"/"suspend"/"hibernate" from the callers).
 * On failure the backend's first stderr line is copied into err
 * (truncated, always NUL-terminated) for display to the user.
 * Returns true iff the command exited 0. */
bool xw_power_exec(const char *verb, char *err, size_t errlen);

#endif /* XW_POWER_H */
