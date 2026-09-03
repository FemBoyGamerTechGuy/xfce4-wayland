/* xw-ctl.h — session control-socket client helper shared by the
 * graphical clients (xw-exit, xw-panel): one request, one reply line.
 *
 * The protocol is documented in ARCHITECTURE.md (private unix line
 * protocol on $XDG_RUNTIME_DIR/xw-session.sock).
 */
#ifndef XW_CTL_H
#define XW_CTL_H

#include <stdbool.h>
#include <stddef.h>

/* Send one command to the session manager; reply (first line, newline
 * stripped) lands in reply.  Returns true iff the manager answered
 * "ok".  reply is always NUL-terminated and carries the error text on
 * failure ("no session manager (...)" etc.) so callers can log it. */
bool xw_ctl_send(const char *cmd, char *reply, size_t reply_len);

/* Fire-and-forget variant for clients that must never block inside
 * their Wayland dispatch (the panel: a ctl round trip inside a button
 * handler stalls every frame callback, hover redraw and configure
 * while the session manager finishes its 500ms poll cycle — the
 * "clicked Start, everything froze" failure mode). Forks a child
 * that performs the round trip and exits; the caller reaps it via
 * its own SIGCHLD handler (the panel installs one; this function
 * deliberately does not touch signal dispositions — xw-exit uses
 * the blocking variant in a process that owns nothing else).
 * Returns true iff the child was spawned. */
bool xw_ctl_send_async(const char *cmd);

#endif /* XW_CTL_H */
